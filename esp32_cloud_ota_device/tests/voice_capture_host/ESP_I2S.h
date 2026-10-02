#pragma once

#include "Arduino.h"
#include "fake_hardware.h"

using esp_err_t = int;
using i2s_chan_handle_t = void *;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr int I2S_NUM_0 = 0;
constexpr int I2S_MODE_STD = 0;
constexpr int I2S_DATA_BIT_WIDTH_32BIT = 32;
constexpr int I2S_SLOT_MODE_STEREO = 2;

inline esp_err_t i2s_channel_read(i2s_chan_handle_t, void *out, size_t wanted,
                                size_t *read, uint32_t) {
  ++FakeHardware::readCalls;
  if (FakeHardware::inputSamples >= FakeHardware::failReadAt) {
    *read = 0;
    return ESP_FAIL;
  }
  const size_t frames = min(wanted / (2 * sizeof(int32_t)),FakeHardware::maxReadFrames);
  auto *raw = static_cast<int32_t *>(out);
  for (size_t i = 0; i < frames; ++i) {
    const int16_t value = FakeHardware::sampleAt ? FakeHardware::sampleAt(FakeHardware::inputSamples + i) : 0;
    raw[2 * i] = static_cast<int32_t>(value) * 65536;
    raw[2 * i + 1] = 0;
  }
  FakeHardware::inputSamples += frames;
  fakeMillis += static_cast<uint32_t>(frames * 1000 / 16000);
  *read = frames * 2 * sizeof(int32_t);
  return ESP_OK;
}

class I2SClass {
 public:
  explicit I2SClass(int) {}
  void setPins(int, int, int, int) {}
  bool begin(int, uint32_t, int, int) { return !FakeHardware::failI2sBegin; }
  bool end() { return true; }
  int lastError() const { return -1; }
  i2s_chan_handle_t rxChan() { return reinterpret_cast<void *>(1); }
  size_t write(const void *, size_t length) {
    if (FakeHardware::failI2sWrite) return 0;
    FakeHardware::writtenFrames += length / (2 * sizeof(int32_t));
    return length;
  }
};
