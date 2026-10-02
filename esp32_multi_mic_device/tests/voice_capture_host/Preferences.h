#pragma once
#include <map>
#include <string>
#include <cstdint>
namespace FakePreferences {
inline std::map<std::string, uint16_t> values;
inline bool failBegin = false, failWrite = false;
}
class Preferences {
 public:
  bool begin(const char *, bool = false) { return !FakePreferences::failBegin; }
  void end() {}
  uint16_t getUShort(const char *key, uint16_t fallback = 0) {
    auto found = FakePreferences::values.find(key);
    return found == FakePreferences::values.end() ? fallback : found->second;
  }
  size_t putUShort(const char *key, uint16_t value) {
    if (FakePreferences::failWrite) return 0;
    FakePreferences::values[key] = value;
    return sizeof(value);
  }
};
