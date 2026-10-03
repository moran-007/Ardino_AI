#pragma once
#include <cstddef>
inline void *(*fakeTlsAllocate)(size_t, size_t) = nullptr;
inline void (*fakeTlsRelease)(void *) = nullptr;
inline int mbedtls_platform_set_calloc_free(void *(*allocate)(size_t, size_t), void (*release)(void *)) {
  fakeTlsAllocate = allocate; fakeTlsRelease = release; return 0;
}
