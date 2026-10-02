#pragma once
#include <Arduino.h>

namespace MultiMicConfig {
bool begin();
bool busy();
bool apply(uint8_t mask, uint8_t preferred, String &message);
bool executeCommand(const String &command, String &message);
}
