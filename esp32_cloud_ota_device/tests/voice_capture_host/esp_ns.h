#pragma once
#include "fake_hardware.h"
using ns_handle_t = void *;
inline ns_handle_t ns_pro_create(int frame, int, int rate) {
  if (frame != 10 || rate != 16000 || FakeHardware::failNs) return nullptr;
  ++FakeHardware::liveNs;
  return reinterpret_cast<void *>(1);
}
inline void ns_process(ns_handle_t, int16_t *input, int16_t *output) {
  ++FakeHardware::nsCalls;
  for (size_t i=0;i<160;++i) output[i]=input[i]/FakeHardware::nsDivisor;
}
inline void ns_destroy(ns_handle_t) { --FakeHardware::liveNs; }
