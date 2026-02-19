# Zebra-Scent

ESP8266 (D1 mini) firmware to control up to 8 diffusers via a 74HC595 shift register.

## Features

- 8 channel output control (single channel + master on/off)
- Web UI for:
  - live status
  - channel names
  - schedules
  - MQTT settings
- MQTT integration with retained states
- Home Assistant auto-discovery
- WiFi onboarding via WiFiManager AP portal
- Persistent settings and schedules in LittleFS

## Project structure

```text
Zebra_Scent/
├── Zebra_Scent.ino   # Main application logic (setup, loop, APIs, MQTT, scheduling)
├── AppConfig.h       # Shared constants, pin mapping, config/schedule structs
├── AppConfig.cpp     # Global config and schedule storage instances
├── WebUi.h           # Web UI declaration
└── WebUi.cpp         # Embedded HTML/CSS/JS Web UI
```

## Requirements

- Board: ESP8266 (e.g. Wemos D1 mini)
- Arduino IDE 2.x (or PlatformIO with equivalent libraries)
- Libraries:
  - ESP8266 core libraries (`ESP8266WiFi`, `ESP8266WebServer`)
  - `LittleFS`
  - `WiFiManager`
  - `PubSubClient`
  - `ArduinoJson`

## Build & flash (Arduino IDE)

1. Open `Zebra_Scent/Zebra_Scent.ino` in Arduino IDE.
2. Select your ESP8266 board and serial port.
3. Ensure required libraries are installed.
4. Compile and upload.

## First start

1. Device starts a WiFiManager portal if no credentials exist.
2. Connect to AP `ZebraScent-<chipid>`.
3. Configure WiFi (and optional MQTT defaults).
4. Open device IP in browser to use the Web UI.

## Notes

- Time zone is configured to `Europe/Berlin` in firmware.
- MQTT is optional; leave host empty to disable.
