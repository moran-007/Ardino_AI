#pragma once
#include "Arduino.h"
#include <map>
inline bool failInteractionOpen = false, failInteractionWrite = false;
inline size_t interactionNvsWrites = 0;
inline std::map<std::string, std::string> interactionNvs;
class Preferences {
 public:
  bool begin(const char *name, bool = false) { space = name;return !failInteractionOpen; }
  void end() {}
  String getString(const char *key, const char *fallback = "") const {
    auto item = interactionNvs.find(space + "/" + key);return item == interactionNvs.end() ? String(fallback) : String(item->second);
  }
  size_t putString(const char *key, const String &value) {
    if (failInteractionWrite) return 0;
    interactionNvs[space + "/" + key] = value.c_str();++interactionNvsWrites;return value.length();
  }
 private:
  std::string space;
};
