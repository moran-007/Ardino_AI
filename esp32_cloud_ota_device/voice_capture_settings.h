#pragma once

#include <ArduinoJson.h>
#include <stdint.h>

namespace VoiceCapture {
struct Settings {
  uint8_t gain = 5;
  uint8_t sensitivity = 50;
  bool noiseSuppression = true;
  uint16_t silenceMs = 1200;
  uint8_t listenSeconds = 8;
  bool continuous = false;
};

const Settings &settings();
bool begin();
bool save(const Settings &candidate);
bool parse(JsonVariantConst document, Settings &candidate);
void appendStatus(JsonObject doc);
}  // namespace VoiceCapture
