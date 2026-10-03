#pragma once
#include <cstdlib>
#include "fake_hardware.h"
constexpr unsigned MALLOC_CAP_SPIRAM = 1;
constexpr unsigned MALLOC_CAP_8BIT = 2;
inline void *heap_caps_malloc(size_t bytes, unsigned caps) {
  FakeHardware::captureAllocationCaps = caps;
  if (FakeHardware::failCaptureAllocation) return nullptr;
  return std::malloc(bytes);
}
inline void *heap_caps_aligned_alloc(size_t, size_t bytes, unsigned caps) {
  return heap_caps_malloc(bytes,caps);
}
