#pragma once
#define ledcWrite oldInteractionHostWrite
#include "../display_host/Arduino.h"
#undef ledcWrite
#include <vector>
#include <utility>
inline std::vector<std::pair<uint8_t, uint32_t>> servoWrites;
inline bool ledcWrite(uint8_t pin, uint32_t duty) {
  if (fakePwmFailure) return false;
  servoWrites.emplace_back(pin, duty);fakeDuty = duty;return true;
}
