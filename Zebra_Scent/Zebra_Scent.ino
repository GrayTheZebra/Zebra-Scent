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

#include "AppConfig.h"
#include "WebUi.h"

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

// ===================== Web UI =====================
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
