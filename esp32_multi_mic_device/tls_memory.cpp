#include "tls_memory.h"
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <stdint.h>

namespace TlsMemory {
namespace {
void *allocate(size_t count, size_t size) {
  if (size && count > SIZE_MAX / size) return nullptr;
  const size_t bytes = count * size;
  // TLS records dominate connection RAM. ESP-IDF supports external Mbed TLS
  // allocations; small crypto structures stay internal for faster access.
  const unsigned first = (bytes >= 4096 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
  const unsigned fallback = (bytes >= 4096 ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT;
  void *memory = heap_caps_calloc(count, size, first);
  return memory ? memory : heap_caps_calloc(count, size, fallback);
}
void release(void *memory) { heap_caps_free(memory); }
}
bool begin() {
  if (!psramFound()) return false;
  return mbedtls_platform_set_calloc_free(allocate, release) == 0;
}
}
