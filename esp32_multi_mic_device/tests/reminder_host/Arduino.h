#pragma once
#include "../display_host/Arduino.h"
inline bool isDigit(unsigned char value) { return std::isdigit(value)!=0; }
#ifdef _WIN32
inline tm *localtime_r(const time_t *value, tm *output) {
  const tm *result=std::localtime(value);if(!result)return nullptr;*output=*result;return output;
}
#endif
