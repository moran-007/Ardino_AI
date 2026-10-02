#pragma once

#include "Arduino.h"
#include "fake_hardware.h"

using esp_err_t = int;
using i2s_chan_handle_t = void *;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr int I2S_NUM_0 = 0;
constexpr int I2S_NUM_1 = 1;
constexpr int I2S_MODE_STD = 0;
constexpr int I2S_DATA_BIT_WIDTH_32BIT = 32;
constexpr int I2S_SLOT_MODE_STEREO = 2;

inline esp_err_t i2s_channel_read(i2s_chan_handle_t handle, void *out, size_t wanted,
                                size_t *read, uint32_t) {
  const int port = handle == reinterpret_cast<void *>(2) ? 1 : 0;
  auto &cursor = port ? FakeHardware::thirdInputSamples : FakeHardware::inputSamples;
  ++(port ? FakeHardware::thirdReadCalls : FakeHardware::readCalls);
  if (cursor >= (port ? FakeHardware::thirdFailReadAt : FakeHardware::failReadAt)) {
    *read = 0;
    return ESP_FAIL;
  }
  const size_t frames = min(wanted / (2 * sizeof(int32_t)),FakeHardware::maxReadFrames);
  auto *raw = static_cast<int32_t *>(out);
  for (size_t i = 0; i < frames; ++i) {
    auto &source = port ? FakeHardware::thirdSampleAt : FakeHardware::sampleAt;
    const int16_t value = source ? source(cursor + i) : 0;
    raw[2 * i] = static_cast<int32_t>(value) * 65536;
    raw[2 * i + 1] = !port && FakeHardware::rightSampleAt
        ? static_cast<int32_t>(FakeHardware::rightSampleAt(cursor + i)) * 65536 : 0;
  }
  cursor += frames;
  if (!port) fakeMillis += static_cast<uint32_t>(frames * 1000 / 16000);
  *read = frames * 2 * sizeof(int32_t) - (FakeHardware::malformedPort == port ? 1 : 0);
  return ESP_OK;
}

class I2SClass {
 public:
  explicit I2SClass(int port) : port_(port) {}
  void setPins(int bclk, int ws, int dout, int din) {
    auto &pins = FakeHardware::portPins[port_];
    pins[0]=bclk; pins[1]=ws; pins[2]=dout; pins[3]=din;
  }
  bool begin(int, uint32_t, int, int) {
    ++FakeHardware::beginCalls[port_];
    return FakeHardware::portActive[port_] =
        !(FakeHardware::failI2sBegin || (port_ && FakeHardware::failThirdBegin));
  }
  bool end() {
    ++FakeHardware::endCalls[port_];
    FakeHardware::portActive[port_] = false;
    return true;
  }
  int lastError() const { return -1; }
  i2s_chan_handle_t rxChan() { return reinterpret_cast<void *>(port_ + 1); }
  size_t write(const void *, size_t length) {
    if (FakeHardware::failI2sWrite) return 0;
    FakeHardware::writtenFrames += length / (2 * sizeof(int32_t));
    return length;
  }
 private:
  int port_;
};
