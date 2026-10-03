#pragma once
#include "../music_host/Arduino.h"
inline uint32_t micros() { return millis() * 1000U; }
