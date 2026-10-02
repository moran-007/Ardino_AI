#pragma once
#include <Arduino.h>

namespace RobotInteraction {
struct Config {
  bool enabled = false;
  uint8_t channel = 1;
  uint8_t lowDegrees = 0;
  uint8_t highDegrees = 20;
};
bool begin();
bool configure(const Config &config);
String toJson();
bool onReply(uint8_t mood);
void poll();
void stop(bool restore = true);
bool busy();
const char *lastError();
}  // namespace RobotInteraction
