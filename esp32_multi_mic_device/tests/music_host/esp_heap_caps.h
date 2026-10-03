#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
constexpr unsigned MALLOC_CAP_INTERNAL = 4;
inline size_t fakeFreeInternal = 100000, fakeLargestInternal = 32768;
inline unsigned fakeHeapFailureCaps = 0, fakeLastHeapCaps = 0;
inline size_t heap_caps_get_free_size(unsigned) { return fakeFreeInternal; }
inline size_t heap_caps_get_largest_free_block(unsigned) { return fakeLargestInternal; }
inline void *heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void *heap_caps_calloc(size_t n, size_t size, unsigned caps) {
  fakeLastHeapCaps = caps;
  return (caps & fakeHeapFailureCaps) ? nullptr : std::calloc(n, size);
}
inline void heap_caps_free(void *pointer) { std::free(pointer); }
inline void *heap_caps_realloc(void *pointer, size_t size, unsigned) { return std::realloc(pointer, size); }
