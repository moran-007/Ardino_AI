#include "../../tls_memory.cpp"
#include <cassert>
#include <iostream>
int main() {
  fakePsramAvailable = false;
  assert(!TlsMemory::begin() && !fakeTlsAllocate);
  fakePsramAvailable = true; assert(TlsMemory::begin());
  auto *small = fakeTlsAllocate(1, 64);
  assert(small && fakeLastHeapCaps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  fakeTlsRelease(small);
  auto *record = fakeTlsAllocate(1, 16384);
  assert(record && fakeLastHeapCaps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  for (size_t i = 0; i < 16384; ++i) assert(static_cast<unsigned char *>(record)[i] == 0);
  fakeTlsRelease(record);
  fakeHeapFailureCaps = MALLOC_CAP_SPIRAM;
  record = fakeTlsAllocate(1, 16384);
  assert(record && fakeLastHeapCaps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); fakeTlsRelease(record);
  fakeHeapFailureCaps = MALLOC_CAP_INTERNAL;
  small = fakeTlsAllocate(1, 64);
  assert(small && fakeLastHeapCaps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)); fakeTlsRelease(small);
  assert(!fakeTlsAllocate(SIZE_MAX, 2));
  fakeHeapFailureCaps = MALLOC_CAP_INTERNAL | MALLOC_CAP_SPIRAM;
  assert(!fakeTlsAllocate(1, 16384));
  std::cout << "PASS: TLS allocator placement/zeroing/overflow/failure/fallback/no-PSRAM checks\n";
}
