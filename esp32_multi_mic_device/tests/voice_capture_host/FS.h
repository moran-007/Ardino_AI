#pragma once

#include "Arduino.h"
#include "fake_hardware.h"

#include <memory>

#define FILE_WRITE "w"
#define FILE_READ "r"

class File {
 public:
  File() = default;
  File(const std::string &path, bool write) : state_(std::make_shared<State>()) {
    state_->path = path;
    state_->write = write;
  }
  explicit operator bool() const { return state_ && !state_->closed; }
  size_t write(const uint8_t *data, size_t length) {
    if (!*this || !state_->write) return 0;
    if (!FakeHardware::fileWriteCalls++) FakeHardware::firstFileWriteInput = FakeHardware::inputSamples;
    delay(FakeHardware::fileWriteDelayMs);
    auto &bytes = FakeHardware::files[state_->path];
    if (bytes.size() >= FakeHardware::failFileWriteAfter) return 0;
    const size_t count = min(length, FakeHardware::failFileWriteAfter - bytes.size());
    bytes.insert(bytes.end(), data, data + count);
    state_->position += count;
    return count;
  }
  size_t read(uint8_t *out, size_t length) {
    if (!*this) return 0;
    auto &bytes = FakeHardware::files[state_->path];
    const size_t count = min(length, bytes.size() - state_->position);
    std::memcpy(out, bytes.data() + state_->position, count);
    state_->position += count;
    return count;
  }
  size_t size() const { return *this ? FakeHardware::files[state_->path].size() : 0; }
  bool available() const { return *this && state_->position < size(); }
  void flush() {}
  void close() { if (state_) state_->closed = true; }

 private:
  struct State {
    std::string path;
    size_t position = 0;
    bool write = false;
    bool closed = false;
  };
  std::shared_ptr<State> state_;
};

namespace fs {
class FS {
 public:
  bool begin(bool) { return true; }
  File open(const char *path, const char *mode) {
    if (FakeHardware::failOpen) return {};
    const bool write = std::strcmp(mode, FILE_WRITE) == 0;
    if (!write && !exists(path)) return {};
    if (write) FakeHardware::files[path].clear();
    return File(path, write);
  }
  bool remove(const char *path) { return FakeHardware::files.erase(path) > 0; }
  bool exists(const char *path) const { return FakeHardware::files.count(path) != 0; }
};
}
