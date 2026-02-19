/* Zebra-Scent Host (ESP8266 / D1 mini) – FULL (Names + Master + HA Discovery) – UI Focus FIX
   - Fix: WebUI inputs keep focus (names + mqtt fields are not re-rendered every refresh)
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ===================== Pins (D1 mini) =====================
#define PIN_DATA   D7
#define PIN_CLOCK  D5
#define PIN_LATCH  D6
#define PIN_OE     D0   // OE active LOW

// ===================== Files =====================
static const char* CFG_FILE   = "/zebrascent.json";
static const char* SCHED_FILE = "/schedules.json";

// ===================== Config =====================
struct Config {
  char mqttHost[64] = "";
  uint16_t mqttPort = 1883;
  char mqttUser[64] = "";
  char mqttPass[64] = "";
  char baseTopic[64] = "zebrascent";
  char haPrefix[64]  = "homeassistant";
  char chName[8][32] = {
    "Diffuser 1","Diffuser 2","Diffuser 3","Diffuser 4",
    "Diffuser 5","Diffuser 6","Diffuser 7","Diffuser 8"
  };
};
Config cfg;

// ===================== Schedules =====================
static const uint8_t MAX_RULES = 16;
struct ScheduleRule {
  bool enabled = false;
  uint8_t channel = 1;
  uint16_t startMin = 0;
  uint16_t endMin = 0;
  uint8_t daysMask = 0x7F;
};
ScheduleRule rules[MAX_RULES];

// ===================== Runtime =====================
ESP8266WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt(espClient);

volatile uint8_t outMask = 0x00;
uint8_t lastWrittenMask = 0xFF;

unsigned long lastMqttAttemptMs = 0;
const unsigned long MQTT_RETRY_MS = 5000;

unsigned long lastStatusPublishMs = 0;
const unsigned long STATUS_PUBLISH_MS = 30000;

unsigned long lastScheduleTickMs = 0;

// ===================== Helpers =====================
static bool parseBoolPayload(const String& s) {
  String v = s; v.trim(); v.toLowerCase();
  if (v == "on" || v == "1" || v == "true" || v == "yes")  return true;
  if (v == "off"|| v == "0" || v == "false"|| v == "no")   return false;
  return (v.toInt() != 0);
}
static String chipIdHex() { String s = String(ESP.getChipId(), HEX); s.toLowerCase(); return s; }
static bool mqttEnabled() { return strlen(cfg.mqttHost) > 0; }
static bool allChannelsOn() { return (outMask == 0xFF); }
static bool anyChannelOn() { return (outMask != 0x00); }

static const char* chDisplayName(uint8_t ch1to8) {
  static char fallback[16];
  if (ch1to8 < 1 || ch1to8 > 8) { snprintf(fallback, sizeof(fallback), "Diffuser ?"); return fallback; }
  const char* n = cfg.chName[ch1to8 - 1];
  if (n && strlen(n) > 0) return n;
  snprintf(fallback, sizeof(fallback), "Diffuser %u", ch1to8);
  return fallback;
}

// Time helpers
static bool getLocalMinutes(uint16_t &minutesOut) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  struct tm lt; localtime_r(&now, &lt);
  minutesOut = (uint16_t)(lt.tm_hour * 60 + lt.tm_min);
  return true;
}
static uint8_t getLocalWeekdayMask() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;
  struct tm lt; localtime_r(&now, &lt);
  int w = lt.tm_wday;               // 0=So..6=Sa
  int idx = (w == 0) ? 6 : (w - 1); // Mo=0..So=6
  return (uint8_t)(1U << idx);
}

// ===================== 74HC595 =====================
static void write595(uint8_t value) {
  digitalWrite(PIN_LATCH, LOW);
  shiftOut(PIN_DATA, PIN_CLOCK, MSBFIRST, value);
  digitalWrite(PIN_LATCH, HIGH);
}
static void applyOutputsIfChanged() {
  if (outMask == lastWrittenMask) return;
  write595(outMask);
  lastWrittenMask = outMask;
}
static void setChannel(uint8_t ch1to8, bool on) {
  if (ch1to8 < 1 || ch1to8 > 8) return;
  uint8_t bit = (1U << (ch1to8 - 1));
  if (on) outMask |= bit;
  else    outMask &= ~bit;
}
static bool getChannel(uint8_t ch1to8) {
  if (ch1to8 < 1 || ch1to8 > 8) return false;
  return (outMask & (1U << (ch1to8 - 1))) != 0;
}
static void setAll(bool on) { outMask = on ? 0xFF : 0x00; }

// ===================== LittleFS Config =====================
static void ensureDefaultsForNames() {
  for (uint8_t i = 0; i < 8; i++) {
    if (strlen(cfg.chName[i]) == 0) {
      char tmp[16];
      snprintf(tmp, sizeof(tmp), "Diffuser %u", (unsigned)(i + 1));
      strlcpy(cfg.chName[i], tmp, sizeof(cfg.chName[i]));
    }
  }
}
static bool loadConfig() {
  if (!LittleFS.begin()) return false;
  if (!LittleFS.exists(CFG_FILE)) { ensureDefaultsForNames(); return false; }
  File f = LittleFS.open(CFG_FILE, "r");
  if (!f) return false;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  strlcpy(cfg.mqttHost,  doc["mqttHost"]  | cfg.mqttHost,  sizeof(cfg.mqttHost));
  cfg.mqttPort = doc["mqttPort"] | cfg.mqttPort;
  strlcpy(cfg.mqttUser,  doc["mqttUser"]  | cfg.mqttUser,  sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,  doc["mqttPass"]  | cfg.mqttPass,  sizeof(cfg.mqttPass));
  strlcpy(cfg.baseTopic, doc["baseTopic"] | cfg.baseTopic, sizeof(cfg.baseTopic));
  strlcpy(cfg.haPrefix,  doc["haPrefix"]  | cfg.haPrefix,  sizeof(cfg.haPrefix));

  JsonArray names = doc["names"].as<JsonArray>();
  if (!names.isNull()) {
    uint8_t idx = 0;
    for (JsonVariant v : names) {
      if (idx >= 8) break;
      const char* s = v.as<const char*>();
      if (s) strlcpy(cfg.chName[idx], s, sizeof(cfg.chName[idx]));
      idx++;
    }
  }

  if (strlen(cfg.baseTopic) == 0) strlcpy(cfg.baseTopic, "zebrascent", sizeof(cfg.baseTopic));
  if (strlen(cfg.haPrefix) == 0)  strlcpy(cfg.haPrefix, "homeassistant", sizeof(cfg.haPrefix));
  if (cfg.mqttPort == 0) cfg.mqttPort = 1883;

  ensureDefaultsForNames();
  return true;
}
static bool saveConfig() {
  if (!LittleFS.begin()) return false;

  if (strlen(cfg.baseTopic) == 0) strlcpy(cfg.baseTopic, "zebrascent", sizeof(cfg.baseTopic));
  if (strlen(cfg.haPrefix) == 0)  strlcpy(cfg.haPrefix, "homeassistant", sizeof(cfg.haPrefix));
  if (cfg.mqttPort == 0) cfg.mqttPort = 1883;

  ensureDefaultsForNames();

  StaticJsonDocument<1024> doc;
  doc["mqttHost"]  = cfg.mqttHost;
  doc["mqttPort"]  = cfg.mqttPort;
  doc["mqttUser"]  = cfg.mqttUser;
  doc["mqttPass"]  = cfg.mqttPass;
  doc["baseTopic"] = cfg.baseTopic;
  doc["haPrefix"]  = cfg.haPrefix;

  JsonArray names = doc.createNestedArray("names");
  for (uint8_t i = 0; i < 8; i++) names.add(cfg.chName[i]);

  File f = LittleFS.open(CFG_FILE, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

// ===================== LittleFS Schedules =====================
static bool loadSchedules() {
  if (!LittleFS.begin()) return false;
  if (!LittleFS.exists(SCHED_FILE)) return false;

  File f = LittleFS.open(SCHED_FILE, "r");
  if (!f) return false;

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, f)) { f.close(); return false; }
  f.close();

  JsonArray arr = doc["rules"].as<JsonArray>();
  uint8_t idx = 0;
  for (JsonObject o : arr) {
    if (idx >= MAX_RULES) break;
    rules[idx].enabled  = o["en"] | false;
    rules[idx].channel  = o["ch"] | 1;
    rules[idx].startMin = o["s"]  | 0;
    rules[idx].endMin   = o["e"]  | 0;
    rules[idx].daysMask = o["d"]  | 0x7F;
    idx++;
  }
  for (; idx < MAX_RULES; idx++) rules[idx].enabled = false;
  return true;
}
static bool saveSchedules() {
  if (!LittleFS.begin()) return false;

  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("rules");
  for (uint8_t i = 0; i < MAX_RULES; i++) {
    JsonObject o = arr.createNestedObject();
    o["en"] = rules[i].enabled;
    o["ch"] = rules[i].channel;
    o["s"]  = rules[i].startMin;
    o["e"]  = rules[i].endMin;
    o["d"]  = rules[i].daysMask;
  }

  File f = LittleFS.open(SCHED_FILE, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}

// ===================== MQTT Topics =====================
static String tStatus() { return String(cfg.baseTopic) + "/status"; }

static String tCmd(uint8_t ch)   { return String(cfg.baseTopic) + "/diffuser/" + String(ch) + "/set"; }
static String tState(uint8_t ch) { return String(cfg.baseTopic) + "/diffuser/" + String(ch) + "/state"; }

static String tAllCmd()   { return String(cfg.baseTopic) + "/all/set"; }
static String tAllState() { return String(cfg.baseTopic) + "/all/state"; }

// HA discovery topics
static String haObjectId(uint8_t ch) { return "zebrascent_" + chipIdHex() + "_diffuser_" + String(ch); }
static String haAllObjectId() { return "zebrascent_" + chipIdHex() + "_all"; }
static String tHaConfig(uint8_t ch) { return String(cfg.haPrefix) + "/switch/" + haObjectId(ch) + "/config"; }
static String tHaAllConfig() { return String(cfg.haPrefix) + "/switch/" + haAllObjectId() + "/config"; }

// ===================== MQTT publish helpers =====================
static void mqttPublishAvailability(const char* v) {
  if (!mqtt.connected()) return;
  String ts = tStatus();
  mqtt.publish(ts.c_str(), v, true);
}
static void mqttPublishState(uint8_t ch) {
  if (!mqtt.connected()) return;
  const char* payload = getChannel(ch) ? "ON" : "OFF";
  mqtt.publish(tState(ch).c_str(), payload, true);
}
static void mqttPublishAllState() {
  if (!mqtt.connected()) return;
  const char* payload = allChannelsOn() ? "ON" : "OFF";
  mqtt.publish(tAllState().c_str(), payload, true);
}
static void mqttPublishAllStates() {
  if (!mqtt.connected()) return;
  for (uint8_t ch = 1; ch <= 8; ch++) mqttPublishState(ch);
  mqttPublishAllState();
}
static void mqttResubscribe() {
  for (uint8_t ch = 1; ch <= 8; ch++) mqtt.subscribe(tCmd(ch).c_str());
  mqtt.subscribe(tAllCmd().c_str());
}

static void mqttPublishHADiscovery() {
  if (!mqtt.connected()) return;

  String id = chipIdHex();
  String nodeId = "zebrascent_" + id;

  // Master "Alle Diffuser"
  {
    String cfgTopic = tHaAllConfig();
    String payload; payload.reserve(700);

    payload += "{";
    payload += "\"name\":\"Alle Diffuser\",";
    payload += "\"unique_id\":\"" + nodeId + "_all\",";
    payload += "\"object_id\":\"" + haAllObjectId() + "\",";

    payload += "\"command_topic\":\"" + tAllCmd() + "\",";
    payload += "\"state_topic\":\"" + tAllState() + "\",";

    payload += "\"payload_on\":\"ON\",";
    payload += "\"payload_off\":\"OFF\",";

    payload += "\"availability_topic\":\"" + tStatus() + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";

    payload += "\"icon\":\"mdi:power\",";
    payload += "\"device\":{";
    payload +=   "\"identifiers\":[\"" + nodeId + "\"],";
    payload +=   "\"name\":\"Zebra-Scent\",";
    payload +=   "\"manufacturer\":\"DIY\",";
    payload +=   "\"model\":\"Zebra-Scent Host (ESP8266 + 74HC595)\"";
    payload += "}";
    payload += "}";

    mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
  }

  // 8 channel switches
  for (uint8_t ch = 1; ch <= 8; ch++) {
    String cfgTopic = tHaConfig(ch);

    String payload; payload.reserve(750);
    payload += "{";
    payload += "\"name\":\"" + String(chDisplayName(ch)) + "\",";
    payload += "\"unique_id\":\"" + nodeId + "_ch" + String(ch) + "\",";
    payload += "\"object_id\":\"" + haObjectId(ch) + "\",";

    payload += "\"command_topic\":\"" + tCmd(ch) + "\",";
    payload += "\"state_topic\":\"" + tState(ch) + "\",";

    payload += "\"payload_on\":\"ON\",";
    payload += "\"payload_off\":\"OFF\",";

    payload += "\"availability_topic\":\"" + tStatus() + "\",";
    payload += "\"payload_available\":\"online\",";
    payload += "\"payload_not_available\":\"offline\",";

    payload += "\"icon\":\"mdi:air-humidifier\",";
    payload += "\"device\":{";
    payload +=   "\"identifiers\":[\"" + nodeId + "\"],";
    payload +=   "\"name\":\"Zebra-Scent\",";
    payload +=   "\"manufacturer\":\"DIY\",";
    payload +=   "\"model\":\"Zebra-Scent Host (ESP8266 + 74HC595)\"";
    payload += "}";
    payload += "}";

    mqtt.publish(cfgTopic.c_str(), payload.c_str(), true);
  }
}

static void onOutputsChangedPublishIfMqtt() {
  if (!mqtt.connected()) return;
  mqttPublishAllStates();
}

// ===================== MQTT callback =====================
static void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t(topic);
  String p; p.reserve(length);
  for (unsigned int i = 0; i < length; i++) p += (char)payload[i];

  if (t == tAllCmd()) {
    setAll(parseBoolPayload(p));
    onOutputsChangedPublishIfMqtt();
    return;
  }

  String base = String(cfg.baseTopic) + "/diffuser/";
  if (!t.startsWith(base)) return;

  String tail = t.substring(base.length());
  int slash = tail.indexOf('/');
  if (slash < 0) return;

  int ch = tail.substring(0, slash).toInt();
  String cmd = tail.substring(slash + 1);
  if (cmd != "set") return;
  if (ch < 1 || ch > 8) return;

  setChannel((uint8_t)ch, parseBoolPayload(p));
  onOutputsChangedPublishIfMqtt();
}

// ===================== MQTT connect loop =====================
static void ensureMqttConnectedNonBlocking() {
  if (!mqttEnabled()) return;
  if (mqtt.connected()) return;

  unsigned long now = millis();
  if (now - lastMqttAttemptMs < MQTT_RETRY_MS) return;
  lastMqttAttemptMs = now;

  mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  mqtt.setCallback(mqttCallback);

  String willTopic = tStatus();
  String clientId = "ZebraScent-" + chipIdHex();

  bool ok = false;
  if (strlen(cfg.mqttUser) > 0) {
    ok = mqtt.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPass,
                      willTopic.c_str(), 0, true, "offline");
  } else {
    ok = mqtt.connect(clientId.c_str(),
                      willTopic.c_str(), 0, true, "offline");
  }

  if (ok) {
    mqttResubscribe();
    mqttPublishAvailability("online");
    mqttPublishHADiscovery();
    mqttPublishAllStates();
  }
}

// ===================== Schedules Runtime =====================
static bool ruleActiveNow(const ScheduleRule &r, uint16_t nowMin, uint8_t todayMask) {
  if (!r.enabled) return false;
  if (r.channel < 1 || r.channel > 8) return false;
  if ((r.daysMask & todayMask) == 0) return false;
  if (r.startMin == r.endMin) return false;

  if (r.startMin < r.endMin) return (nowMin >= r.startMin && nowMin < r.endMin);
  return (nowMin >= r.startMin || nowMin < r.endMin);
}

static void applySchedulesNonBlocking() {
  unsigned long nowMs = millis();
  if (nowMs - lastScheduleTickMs < 1000) return;
  lastScheduleTickMs = nowMs;

  uint16_t nowMin;
  if (!getLocalMinutes(nowMin)) return;

  uint8_t todayMask = getLocalWeekdayMask();
  if (todayMask == 0) return;

  uint8_t scheduleMask = 0;
  uint8_t scheduleDefined = 0;

  for (uint8_t i = 0; i < MAX_RULES; i++) {
    const auto &r = rules[i];
    if (!r.enabled) continue;
    uint8_t bit = (1U << (r.channel - 1));
    scheduleDefined |= bit;
    if (ruleActiveNow(r, nowMin, todayMask)) scheduleMask |= bit;
  }

  uint8_t newMask = outMask;
  newMask &= ~scheduleDefined;
  newMask |= (scheduleMask & scheduleDefined);

  if (newMask != outMask) {
    outMask = newMask;
    onOutputsChangedPublishIfMqtt();
  }
}

// ===================== Web API =====================
static void handleApiState() {
  StaticJsonDocument<1024> doc;
  doc["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  doc["mask"] = outMask;
  doc["allOn"] = allChannelsOn();
  doc["anyOn"] = anyChannelOn();
  uint16_t nowMin;
  bool ok = getLocalMinutes(nowMin);
  doc["timeOk"] = ok;
  if (ok) doc["nowMin"] = nowMin;
  doc["mqttEnabled"] = mqttEnabled();
  doc["mqttConnected"] = mqtt.connected();

  JsonArray names = doc.createNestedArray("names");
  for (uint8_t i = 0; i < 8; i++) names.add(cfg.chName[i]);

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleApiSet() {
  if (!server.hasArg("ch") || !server.hasArg("v")) { server.send(400, "text/plain", "missing ch/v"); return; }
  int ch = server.arg("ch").toInt();
  bool on = parseBoolPayload(server.arg("v"));

  if (ch == 0) setAll(on);
  else {
    if (ch < 1 || ch > 8) { server.send(400, "text/plain", "bad ch"); return; }
    setChannel((uint8_t)ch, on);
  }

  onOutputsChangedPublishIfMqtt();
  server.send(200, "text/plain", "OK");
}

static void handleApiNamesGet() {
  StaticJsonDocument<512> doc;
  JsonArray names = doc.createNestedArray("names");
  for (uint8_t i = 0; i < 8; i++) names.add(cfg.chName[i]);
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleApiNamesPost() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "missing body"); return; }
  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "bad json"); return; }

  JsonArray arr = doc["names"].as<JsonArray>();
  if (arr.isNull()) { server.send(400, "text/plain", "missing names"); return; }

  uint8_t idx = 0;
  for (JsonVariant v : arr) {
    if (idx >= 8) break;
    const char* s = v.as<const char*>();
    if (s) {
      String tmp = String(s); tmp.trim();
      if (!tmp.length()) tmp = "Diffuser " + String(idx + 1);
      strlcpy(cfg.chName[idx], tmp.c_str(), sizeof(cfg.chName[idx]));
    }
    idx++;
  }

  ensureDefaultsForNames();
  saveConfig();

  if (mqtt.connected()) mqttPublishHADiscovery();

  server.send(200, "text/plain", "OK");
}

static void handleApiSchedulesGet() {
  StaticJsonDocument<2048> doc;
  JsonArray arr = doc.createNestedArray("rules");
  for (uint8_t i = 0; i < MAX_RULES; i++) {
    JsonObject o = arr.createNestedObject();
    o["id"] = i;
    o["en"] = rules[i].enabled;
    o["ch"] = rules[i].channel;
    o["s"]  = rules[i].startMin;
    o["e"]  = rules[i].endMin;
    o["d"]  = rules[i].daysMask;
  }
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleApiSchedulesPost() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "missing body"); return; }
  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "bad json"); return; }

  JsonArray arr = doc["rules"].as<JsonArray>();
  uint8_t idx = 0;
  for (JsonObject o : arr) {
    if (idx >= MAX_RULES) break;
    rules[idx].enabled  = o["en"] | false;
    rules[idx].channel  = o["ch"] | 1;
    rules[idx].startMin = o["s"]  | 0;
    rules[idx].endMin   = o["e"]  | 0;
    rules[idx].daysMask = o["d"]  | 0x7F;
    idx++;
  }
  for (; idx < MAX_RULES; idx++) rules[idx].enabled = false;
  saveSchedules();
  server.send(200, "text/plain", "OK");
}

static void handleApiMqttConfigGet() {
  StaticJsonDocument<768> doc;
  doc["mqttHost"]  = cfg.mqttHost;
  doc["mqttPort"]  = cfg.mqttPort;
  doc["mqttUser"]  = cfg.mqttUser;
  doc["baseTopic"] = cfg.baseTopic;
  doc["haPrefix"]  = cfg.haPrefix;
  String out; serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handleApiMqttConfigPost() {
  if (!server.hasArg("plain")) { server.send(400, "text/plain", "missing body"); return; }
  StaticJsonDocument<768> doc;
  if (deserializeJson(doc, server.arg("plain"))) { server.send(400, "text/plain", "bad json"); return; }

  if (doc.containsKey("mqttHost"))  strlcpy(cfg.mqttHost,  doc["mqttHost"].as<const char*>(),  sizeof(cfg.mqttHost));
  if (doc.containsKey("mqttPort"))  cfg.mqttPort = (uint16_t)doc["mqttPort"].as<int>();
  if (doc.containsKey("mqttUser"))  strlcpy(cfg.mqttUser,  doc["mqttUser"].as<const char*>(),  sizeof(cfg.mqttUser));
  if (doc.containsKey("mqttPass"))  strlcpy(cfg.mqttPass,  doc["mqttPass"].as<const char*>(),  sizeof(cfg.mqttPass));
  if (doc.containsKey("baseTopic")) strlcpy(cfg.baseTopic, doc["baseTopic"].as<const char*>(), sizeof(cfg.baseTopic));
  if (doc.containsKey("haPrefix"))  strlcpy(cfg.haPrefix,  doc["haPrefix"].as<const char*>(),  sizeof(cfg.haPrefix));

  if (strlen(cfg.baseTopic) == 0) strlcpy(cfg.baseTopic, "zebrascent", sizeof(cfg.baseTopic));
  if (strlen(cfg.haPrefix) == 0)  strlcpy(cfg.haPrefix, "homeassistant", sizeof(cfg.haPrefix));
  if (cfg.mqttPort == 0) cfg.mqttPort = 1883;

  saveConfig();
  mqtt.disconnect(); // force reconnect -> discovery republish
  server.send(200, "text/plain", "OK");
}

static void handleWifiReset() {
  server.send(200, "text/plain", "WiFi credentials cleared. Rebooting into AP...");
  delay(200);
  WiFiManager wm;
  wm.resetSettings();
  ESP.restart();
}

// ===================== Web UI (FIX: do NOT re-render inputs on every refresh) =====================
static const char UI_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Zebra-Scent</title>
<style>
  :root{--bg:#0b0f14;--card:#121a24;--muted:#93a4b8;--text:#e7eef8;--ok:#22c55e;--bad:#ef4444;}
  body{margin:0;font-family:system-ui,Segoe UI,Roboto,Arial;background:var(--bg);color:var(--text);}
  header{padding:16px 18px;border-bottom:1px solid #1f2a37;display:flex;gap:12px;align-items:center;justify-content:space-between;}
  h1{font-size:18px;margin:0;letter-spacing:.3px;}
  .wrap{padding:18px;max-width:980px;margin:0 auto;}
  .grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px;}
  .card{background:var(--card);border:1px solid #1f2a37;border-radius:14px;padding:14px;box-shadow:0 8px 20px rgba(0,0,0,.25);}
  .row{display:flex;justify-content:space-between;align-items:center;gap:10px;}
  .muted{color:var(--muted);font-size:13px;}
  .pill{font-size:12px;padding:4px 8px;border-radius:999px;border:1px solid #243244;color:var(--muted);}
  .pill.ok{color:var(--ok);border-color:rgba(34,197,94,.35);}
  .pill.bad{color:var(--bad);border-color:rgba(239,68,68,.35);}
  button{background:#0f172a;color:var(--text);border:1px solid #263446;border-radius:12px;padding:10px 12px;cursor:pointer;}
  button:hover{border-color:#3b82f6;}
  button.on{background:rgba(34,197,94,.18);border-color:rgba(34,197,94,.35);}
  button.off{background:rgba(239,68,68,.12);border-color:rgba(239,68,68,.25);}
  .btnrow{display:flex;gap:10px;flex-wrap:wrap;margin-top:10px;}
  input,select{background:#0f172a;color:var(--text);border:1px solid #263446;border-radius:10px;padding:10px;width:100%;}
  .table{width:100%;border-collapse:collapse;}
  .table td,.table th{border-bottom:1px solid #1f2a37;padding:10px 8px;text-align:left;font-size:13px;}
  .table th{color:var(--muted);font-weight:600;}
  footer{padding:14px 18px;color:var(--muted);font-size:12px;}
  code{color:#cbd5e1;}
</style>
</head><body>
<header>
  <h1>Zebra-Scent</h1>
  <div class="row">
    <span class="pill" id="ip">IP: …</span>
    <span class="pill" id="mqtt">MQTT: …</span>
    <span class="pill" id="time">Time: …</span>
  </div>
</header>

<div class="wrap">
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:800">Alle Diffuser</div>
        <div class="muted" id="allStatus">—</div>
      </div>
      <div class="btnrow">
        <button class="on" onclick="setAll(1)">Alles AN</button>
        <button class="off" onclick="setAll(0)">Alles AUS</button>
      </div>
    </div>
  </div>

  <div style="height:14px"></div>
  <div class="grid" id="cards"></div>

  <div style="height:14px"></div>
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:700">Namen</div>
        <div class="muted">Nur gespeichert wenn du auf “Namen speichern” klickst.</div>
      </div>
      <div class="btnrow">
        <button onclick="saveNames()">Namen speichern</button>
        <button onclick="loadNames()">Neu laden</button>
      </div>
    </div>
    <div style="height:10px"></div>
    <div class="grid" id="nameGrid" style="grid-template-columns:repeat(auto-fit,minmax(240px,1fr));"></div>
  </div>

  <div style="height:14px"></div>
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:700">Zeitpläne</div>
        <div class="muted">“Ausgang X von HH:MM bis HH:MM an” • täglich oder nach Wochentagen</div>
      </div>
      <button onclick="addRule()">+ Regel</button>
    </div>
    <div style="height:10px"></div>
    <table class="table" id="schedTable">
      <thead><tr><th>#</th><th>Aktiv</th><th>Kanal</th><th>Von</th><th>Bis</th><th>Tage</th><th></th></tr></thead>
      <tbody></tbody>
    </table>
    <div class="btnrow">
      <button onclick="saveAllSchedules()">Speichern</button>
      <button onclick="reloadSchedules()">Neu laden</button>
    </div>
  </div>

  <div style="height:14px"></div>
  <div class="card">
    <div class="row">
      <div>
        <div style="font-weight:700">Einstellungen</div>
        <div class="muted">MQTT bearbeiten • WLAN neu einrichten (AP via WiFiManager)</div>
      </div>
      <div class="btnrow">
        <button onclick="saveMqtt()">MQTT speichern</button>
        <button onclick="wifiReset()" class="off">WLAN neu</button>
      </div>
    </div>
    <div style="height:10px"></div>
    <div class="grid" style="grid-template-columns:repeat(auto-fit,minmax(240px,1fr));">
      <div><div class="muted">MQTT Host (leer = MQTT aus)</div><input id="mqttHost"></div>
      <div><div class="muted">MQTT Port</div><input id="mqttPort"></div>
      <div><div class="muted">MQTT User</div><input id="mqttUser"></div>
      <div><div class="muted">MQTT Pass (leer = unverändert)</div><input id="mqttPass" type="password"></div>
      <div><div class="muted">BaseTopic</div><input id="baseTopic"></div>
      <div><div class="muted">HA Discovery Prefix</div><input id="haPrefix"></div>
    </div>
  </div>
</div>

<footer>
  Home Assistant: 8 Switches + 1 Master-Switch per MQTT Discovery.
</footer>

<script>
let state={mask:0,timeOk:false,nowMin:0,ip:"",names:[],allOn:false,anyOn:false};
let rules=[];
let namesUiInitialized=false;   // <-- key: init once and do not re-render in refresh loop

function bitOn(mask,idx){return (mask&(1<<idx))!==0;}
function minToHHMM(m){const h=String(Math.floor(m/60)).padStart(2,'0');const mm=String(m%60).padStart(2,'0');return h+":"+mm;}
function hhmmToMin(s){const p=s.split(':');if(p.length!==2)return 0;const h=parseInt(p[0],10),m=parseInt(p[1],10);if(isNaN(h)||isNaN(m))return 0;return (Math.max(0,Math.min(23,h))*60+Math.max(0,Math.min(59,m)));}
function daysToText(mask){const names=["Mo","Di","Mi","Do","Fr","Sa","So"];let out=[];for(let i=0;i<7;i++)if(mask&(1<<i))out.push(names[i]);return out.length?out.join(","):"—";}
async function apiGet(url){const r=await fetch(url);return await r.json();}

async function setCh(ch,v){await fetch(`/api/set?ch=${ch}&v=${v?1:0}`,{method:'POST'});await refreshState();}
async function setAll(v){await fetch(`/api/set?ch=0&v=${v?1:0}`,{method:'POST'});await refreshState();}

function renderCards(){
  const el=document.getElementById('cards');el.innerHTML='';
  for(let i=1;i<=8;i++){
    const on=bitOn(state.mask,i-1);
    const nm=(state.names && state.names[i-1]) ? state.names[i-1] : `Diffuser ${i}`;
    const card=document.createElement('div');
    card.className='card';
    card.innerHTML=`
      <div class="row">
        <div>
          <div style="font-weight:800">${nm}</div>
          <div class="muted">Kanal ${i} • Status: <b>${on?'ON':'OFF'}</b></div>
        </div>
        <button class="${on?'on':'off'}" onclick="setCh(${i},${on?0:1})">${on?'AUS':'AN'}</button>
      </div>
      <div class="btnrow">
        <button onclick="setCh(${i},1)">AN</button>
        <button onclick="setCh(${i},0)">AUS</button>
        <button onclick="setCh(${i},${on?0:1})">Toggle</button>
      </div>`;
    el.appendChild(card);
  }
}

function updateAllStatus(){
  const el = document.getElementById('allStatus');
  if(state.allOn) el.textContent="Status: ALLE AN";
  else if(state.anyOn) el.textContent="Status: TEILWEISE AN";
  else el.textContent="Status: ALLE AUS";
}

function initNamesUI(){
  const g=document.getElementById('nameGrid'); g.innerHTML='';
  for(let i=1;i<=8;i++){
    const div=document.createElement('div');
    div.innerHTML = `
      <div class="muted">Kanal ${i}</div>
      <input id="nm${i}" placeholder="z.B. Rosenduft">`;
    g.appendChild(div);
  }
  namesUiInitialized=true;
}

function fillNamesInputsFromState(){
  for(let i=1;i<=8;i++){
    const el = document.getElementById('nm'+i);
    if(!el) continue;
    const nm=(state.names && state.names[i-1]) ? state.names[i-1] : `Diffuser ${i}`;
    el.value = nm;
  }
}

async function loadNames(){
  const s = await apiGet('/api/names');
  state.names = s.names || state.names;
  if(!namesUiInitialized) initNamesUI();
  fillNamesInputsFromState();
  renderCards();
}

async function saveNames(){
  const arr=[];
  for(let i=1;i<=8;i++){
    const v = document.getElementById('nm'+i).value.trim();
    arr.push(v.length ? v : `Diffuser ${i}`);
  }
  await fetch('/api/names', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({names: arr})});
  await refreshState();
  alert("Gespeichert. HA Discovery wurde (falls MQTT online) neu gesendet.");
}

// Schedules
function renderSchedules(){
  const tb=document.querySelector('#schedTable tbody');tb.innerHTML='';
  rules.forEach((r,idx)=>{
    const tr=document.createElement('tr');
    tr.innerHTML=`
      <td>${idx}</td>
      <td><input type="checkbox" ${r.en?'checked':''} onchange="rules[${idx}].en=this.checked"></td>
      <td><select onchange="rules[${idx}].ch=parseInt(this.value,10)">
        ${Array.from({length:8},(_,i)=>`<option value="${i+1}" ${r.ch==i+1?'selected':''}>${i+1}</option>`).join('')}
      </select></td>
      <td><input value="${minToHHMM(r.s)}" onchange="rules[${idx}].s=hhmmToMin(this.value)"></td>
      <td><input value="${minToHHMM(r.e)}" onchange="rules[${idx}].e=hhmmToMin(this.value)"></td>
      <td><button onclick="toggleDays(${idx})">${daysToText(r.d)}</button></td>
      <td><button onclick="delRule(${idx})">löschen</button></td>`;
    tb.appendChild(tr);
  });
}
function toggleDays(idx){
  const cur=daysToText(rules[idx].d);
  const inp=prompt("Tage (Mo,Di,Mi,Do,Fr,Sa,So) kommasepariert oder 'all'/'none':",cur);
  if(inp===null)return;
  const v=inp.trim().toLowerCase();
  if(v==="all"){rules[idx].d=0x7F;renderSchedules();return;}
  if(v==="none"){rules[idx].d=0x00;renderSchedules();return;}
  const map={mo:0,di:1,mi:2,do:3,fr:4,sa:5,so:6};
  let m=0;v.split(',').map(x=>x.trim()).forEach(x=>{if(map[x]!==undefined)m|=(1<<map[x]);});
  rules[idx].d=m;renderSchedules();
}
function addRule(){if(rules.length>=16){alert("Max 16 Regeln");return;}rules.push({en:true,ch:1,s:8*60,e:9*60,d:0x7F});renderSchedules();}
function delRule(idx){rules.splice(idx,1);renderSchedules();}
async function saveAllSchedules(){
  const fixed=Array.from({length:16},(_,i)=>rules[i]||{en:false,ch:1,s:0,e:0,d:0x7F});
  await fetch('/api/schedules',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({rules:fixed})});
  alert("Gespeichert");
}
async function reloadSchedules(){
  const s=await apiGet('/api/schedules');
  rules=(s.rules||[]).slice(0,16);
  while(rules.length && !rules[rules.length-1].en && rules.length>1) rules.pop();
  renderSchedules();
}

// MQTT config
async function loadMqtt(){
  const c = await apiGet('/api/mqtt');
  mqttHost.value  = c.mqttHost || "";
  mqttPort.value  = c.mqttPort || 1883;
  mqttUser.value  = c.mqttUser || "";
  baseTopic.value = c.baseTopic || "zebrascent";
  haPrefix.value  = c.haPrefix || "homeassistant";
}
async function saveMqtt(){
  const body={
    mqttHost:mqttHost.value.trim(),
    mqttPort:parseInt(mqttPort.value.trim()||"1883",10),
    mqttUser:mqttUser.value.trim(),
    baseTopic:(baseTopic.value.trim()||"zebrascent"),
    haPrefix:(haPrefix.value.trim()||"homeassistant")
  };
  const pass=mqttPass.value; if(pass && pass.length) body.mqttPass=pass;
  await fetch('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  mqttPass.value="";
  alert("Gespeichert. MQTT reconnect + HA Discovery neu.");
}
async function wifiReset(){
  if(!confirm("WLAN wirklich zurücksetzen? Danach startet ein AP zur Neueinrichtung.")) return;
  await fetch('/wifireset',{method:'POST'});
}

async function refreshState(){
  state=await apiGet('/api/state');
  document.getElementById('ip').textContent="IP: "+(state.ip||"—");
  document.getElementById('mqtt').textContent="MQTT: "+(state.mqttEnabled?(state.mqttConnected?"connected":"offline"):"disabled");
  document.getElementById('mqtt').className="pill "+(state.mqttEnabled?(state.mqttConnected?"ok":"bad"):"");
  document.getElementById('time').textContent="Time: "+(state.timeOk?minToHHMM(state.nowMin):"not set");
  document.getElementById('time').className="pill "+(state.timeOk?"ok":"bad");

  updateAllStatus();
  renderCards();

  // IMPORTANT: do NOT touch name inputs here -> keeps focus
  if(!namesUiInitialized){ initNamesUI(); fillNamesInputsFromState(); }
}

(async()=>{
  await loadMqtt();
  await refreshState();
  await loadNames();        // names only loaded once initially
  await reloadSchedules();
  setInterval(refreshState,1500);
})();
</script>
</body></html>
)HTML";

static void handleUi() { server.send(200, "text/html", FPSTR(UI_HTML)); }

// ===================== WiFiManager =====================
static void setupWiFiWithWiFiManager() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  WiFiManagerParameter p_host("mqttHost", "MQTT Host (optional)", cfg.mqttHost, 63);
  char portStr[8]; snprintf(portStr, sizeof(portStr), "%u", cfg.mqttPort);
  WiFiManagerParameter p_port("mqttPort", "MQTT Port", portStr, 7);
  WiFiManagerParameter p_user("mqttUser", "MQTT User", cfg.mqttUser, 63);
  WiFiManagerParameter p_pass("mqttPass", "MQTT Pass", cfg.mqttPass, 63);
  WiFiManagerParameter p_base("baseTopic", "BaseTopic", cfg.baseTopic, 63);
  WiFiManagerParameter p_hap ("haPrefix", "HA Discovery Prefix", cfg.haPrefix, 63);

  wm.addParameter(&p_host);
  wm.addParameter(&p_port);
  wm.addParameter(&p_user);
  wm.addParameter(&p_pass);
  wm.addParameter(&p_base);
  wm.addParameter(&p_hap);

  String apName = "ZebraScent-" + chipIdHex();
  bool ok = wm.autoConnect(apName.c_str());
  if (!ok) ESP.restart();

  Serial.println();
  Serial.printf("[WiFi] SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("[WiFi] IP:   %s\n", WiFi.localIP().toString().c_str());

  strlcpy(cfg.mqttHost, p_host.getValue(), sizeof(cfg.mqttHost));

  uint16_t p = (uint16_t)atoi(p_port.getValue());
  cfg.mqttPort = (p == 0) ? 1883 : p;

  strlcpy(cfg.mqttUser, p_user.getValue(), sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass, p_pass.getValue(), sizeof(cfg.mqttPass));
  strlcpy(cfg.baseTopic, p_base.getValue(), sizeof(cfg.baseTopic));
  strlcpy(cfg.haPrefix,  p_hap.getValue(),  sizeof(cfg.haPrefix));

  if (strlen(cfg.baseTopic) == 0) strlcpy(cfg.baseTopic, "zebrascent", sizeof(cfg.baseTopic));
  if (strlen(cfg.haPrefix) == 0)  strlcpy(cfg.haPrefix, "homeassistant", sizeof(cfg.haPrefix));

  ensureDefaultsForNames();
  saveConfig();
}

// ===================== Setup / Loop =====================
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println(F("Zebra-Scent booting..."));

  pinMode(PIN_DATA, OUTPUT);
  pinMode(PIN_CLOCK, OUTPUT);
  pinMode(PIN_LATCH, OUTPUT);
  pinMode(PIN_OE, OUTPUT);

  digitalWrite(PIN_OE, HIGH); // outputs disabled
  outMask = 0x00;
  write595(outMask);
  lastWrittenMask = outMask;
  digitalWrite(PIN_OE, LOW);  // outputs enabled (still off)

  loadConfig();
  loadSchedules();

  setupWiFiWithWiFiManager();

  // Europe/Berlin time
  setenv("TZ", "CET-1CEST,M3.5.0/02,M10.5.0/03", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  // Web routes
  server.on("/", handleUi);

  server.on("/api/state", HTTP_GET, handleApiState);
  server.on("/api/set", HTTP_POST, handleApiSet);

  server.on("/api/names", HTTP_GET, handleApiNamesGet);
  server.on("/api/names", HTTP_POST, handleApiNamesPost);

  server.on("/api/schedules", HTTP_GET, handleApiSchedulesGet);
  server.on("/api/schedules", HTTP_POST, handleApiSchedulesPost);

  server.on("/api/mqtt", HTTP_GET, handleApiMqttConfigGet);
  server.on("/api/mqtt", HTTP_POST, handleApiMqttConfigPost);

  server.on("/wifireset", HTTP_POST, handleWifiReset);

  server.begin();

  mqtt.setBufferSize(1024);
  mqtt.setCallback(mqttCallback);
  if (mqttEnabled()) mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
}

void loop() {
  server.handleClient();

  applySchedulesNonBlocking();
  applyOutputsIfChanged();

  ensureMqttConnectedNonBlocking();
  if (mqtt.connected()) mqtt.loop();

  unsigned long now = millis();
  if (mqtt.connected() && (now - lastStatusPublishMs >= STATUS_PUBLISH_MS)) {
    lastStatusPublishMs = now;
    mqttPublishAvailability("online");
    mqttPublishAllStates();
  }
}
