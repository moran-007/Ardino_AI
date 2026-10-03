#pragma once
#include "FreeRTOS.h"
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return reinterpret_cast<void *>(1); }
inline int xSemaphoreTake(SemaphoreHandle_t, unsigned) { return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t) {}
