#pragma once
#include "Arduino.h"
#include <map>
#include <memory>
#define FILE_WRITE "w"
#define FILE_READ "r"
inline bool failWakeWrite = false, shortWakeWrite = false, failWakeRename = false;
inline bool failWakeRemoveBackup = false, failWakePromote = false;
inline std::map<std::string, std::shared_ptr<std::string>> wakeFiles;
class File {
 public:
  File() = default;
  File(std::shared_ptr<std::string> value, bool write) : data(value), writable(write) {}
  explicit operator bool() const { return bool(data); }
  size_t size() const { return data ? data->size() : 0; }
  size_t write(const uint8_t *bytes, size_t length) {
    if (!data || !writable || failWakeWrite) return 0;
    if (shortWakeWrite) length /= 2;
    data->append(reinterpret_cast<const char *>(bytes), length); return length;
  }
  size_t readBytes(char *out, size_t length) {
    if (!data) return 0;
    const size_t count = std::min(length, data->size() - position);
    memcpy(out, data->data() + position, count); position += count; return count;
  }
  void flush() {}
  void close() {}
 private:
  std::shared_ptr<std::string> data;
  size_t position = 0;
  bool writable = false;
};
namespace fs {
class FS {
 public:
  File open(const char *path, const char *mode) {
    if (strcmp(mode, FILE_WRITE) == 0) {
      if (failWakeWrite) return File();
      auto value = std::make_shared<std::string>();wakeFiles[path] = value;return File(value, true);
    }
    auto item = wakeFiles.find(path);return item == wakeFiles.end() ? File() : File(item->second, false);
  }
  bool exists(const char *path) { return wakeFiles.count(path) != 0; }
  bool remove(const char *path) {
    if (failWakeRemoveBackup && strcmp(path, "/custom_wake.bak") == 0) return false;
    return wakeFiles.erase(path) != 0;
  }
  bool rename(const char *from, const char *to) {
    auto item = wakeFiles.find(from);
    if (failWakeRename || (failWakePromote && strcmp(from, "/custom_wake.tmp") == 0) ||
        item == wakeFiles.end() || wakeFiles.count(to)) return false;
    wakeFiles[to] = item->second;wakeFiles.erase(item);return true;
  }
};
}
