#pragma once
#include "fake_hardware.h"
enum vad_mode_t { VAD_MODE_0, VAD_MODE_1, VAD_MODE_2, VAD_MODE_3, VAD_MODE_4 };
enum vad_state_t { VAD_SILENCE, VAD_SPEECH };
using vad_handle_t = void *;
inline vad_handle_t vad_create(vad_mode_t mode) {
  FakeHardware::vadMode=mode;
  if (FakeHardware::failVad) return nullptr;
  ++FakeHardware::liveVad;
  return reinterpret_cast<void *>(1);
}
inline vad_state_t vad_process(vad_handle_t, int16_t *, int rate, int ms) {
  if(rate != 16000 || ms != 10) throw std::runtime_error("wrong VAD frame");
  const size_t frame=FakeHardware::vadCalls++;
  return frame<FakeHardware::vadFrames.size() && FakeHardware::vadFrames[frame]
      ? VAD_SPEECH:VAD_SILENCE;
}
inline void vad_destroy(vad_handle_t) { --FakeHardware::liveVad; }
