#pragma once

#include "Arduino.h"
#include <map>

inline std::map<std::string, uint64_t> captureNvs;
inline bool captureNvsBeginFails = false;
inline size_t captureNvsWriteResult = sizeof(uint64_t);
inline size_t captureNvsWriteCount = 0;

class Preferences {
 public:
  bool begin(const char *name, bool) {
    prefix_ = std::string(name) + "/";
    return !captureNvsBeginFails;
  }
  void end() {}
  bool isKey(const char *key) const {
    return captureNvs.count(prefix_ + key) != 0;
  }
  uint64_t getULong64(const char *key, uint64_t fallback) const {
    const auto found = captureNvs.find(prefix_ + key);
    return found == captureNvs.end() ? fallback : found->second;
  }
  size_t putULong64(const char *key, uint64_t value) {
    ++captureNvsWriteCount;
    if (captureNvsWriteResult == sizeof(value)) captureNvs[prefix_ + key] = value;
    return captureNvsWriteResult;
  }

 private:
  std::string prefix_;
};
