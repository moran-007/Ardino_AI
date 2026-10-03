#pragma once
#include <cstdlib>
constexpr unsigned MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
inline void *heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
inline void *heap_caps_calloc(size_t n, size_t size, unsigned) { return std::calloc(n, size); }
inline void *heap_caps_realloc(void *pointer, size_t size, unsigned) { return std::realloc(pointer, size); }
