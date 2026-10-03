#pragma once
#include "FreeRTOS.h"
inline int xTaskCreatePinnedToCore(void (*)(void *), const char *, int, void *, int, TaskHandle_t *task, int) {
  *task = reinterpret_cast<void *>(1); return pdPASS;
}
