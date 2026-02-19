#pragma once

#include <Arduino.h>

// ===================== Pins (D1 mini) =====================
#define PIN_DATA   D7
#define PIN_CLOCK  D5
#define PIN_LATCH  D6
#define PIN_OE     D0   // OE active LOW

// ===================== Files =====================
inline constexpr const char* CFG_FILE   = "/zebrascent.json";
inline constexpr const char* SCHED_FILE = "/schedules.json";

// ===================== Config =====================
struct Config {
  char mqttHost[64] = "";
  uint16_t mqttPort = 1883;
  char mqttUser[64] = "";
  char mqttPass[64] = "";
  char baseTopic[64] = "zebrascent";
  char haPrefix[64]  = "homeassistant";
  char chName[8][32] = {
    "Diffuser 1", "Diffuser 2", "Diffuser 3", "Diffuser 4",
    "Diffuser 5", "Diffuser 6", "Diffuser 7", "Diffuser 8"
  };
};

inline constexpr uint8_t MAX_RULES = 16;

struct ScheduleRule {
  bool enabled = false;
  uint8_t channel = 1;
  uint16_t startMin = 0;
  uint16_t endMin = 0;
  uint8_t daysMask = 0x7F;
};

extern Config cfg;
extern ScheduleRule rules[MAX_RULES];
