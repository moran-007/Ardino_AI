#pragma once
#include "fake_hardware.h"
#include <cstdint>
using TickType_t = uint32_t;
using SemaphoreHandle_t = void *;
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(value) (value)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void *>(1); }
inline int xSemaphoreTake(SemaphoreHandle_t, TickType_t) {
  if (FakeHardware::mutexTaken) return pdFALSE;
  FakeHardware::mutexTaken = true;
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t) { FakeHardware::mutexTaken = false; }
