#pragma once
#define xTaskCreatePinnedToCore oldHostCreateTask
#define vTaskDelay oldHostTaskDelay
#include "../display_host/Arduino.h"
#undef xTaskCreatePinnedToCore
#undef vTaskDelay
#include <stdexcept>
#define PI 3.1415926535897932384626433832795
#define constrain(amt, low, high) ((amt)<(low)?(low):((amt)>(high)?(high):(amt)))
class Stream;
inline void (*wakeTaskFunction)(void *) = nullptr;
inline bool stopWakeTaskAtSleep = false;
inline int xTaskCreatePinnedToCore(void (*task)(void *), const char *, int, void *, int,
                                  TaskHandle_t *handle, int) {
  wakeTaskFunction = task;*handle = reinterpret_cast<void *>(1);return pdPASS;
}
inline void vTaskDelay(unsigned duration) {
  fakeMillis += duration;
  if (stopWakeTaskAtSleep) throw std::runtime_error("host task returned control");
}
inline void taskYIELD() {}
#define CONFIG_MODEL_IN_FLASH 1
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
inline void *heap_caps_malloc(size_t count, unsigned) { return malloc(count); }
