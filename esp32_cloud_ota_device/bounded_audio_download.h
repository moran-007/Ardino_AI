#pragma once
#include <Arduino.h>

namespace AudioDownload {
// SecureClient may report no buffered bytes between TLS records. Its inherited
// readBytes can treat read() == -1 during that gap as EOF, so gate every read on
// current availability and retry short/empty reads until a bounded deadline.
template <typename Input, typename Cancel>
size_t readBounded(Input &input, uint8_t *output, size_t bytes,
                   uint32_t timeoutMs, Cancel cancelled) {
  if (!output || !bytes || !timeoutMs || timeoutMs > 0x7fffffffU) return 0;
  size_t received = 0;
  const uint32_t deadline = millis() + timeoutMs;
  while (received < bytes && static_cast<int32_t>(millis() - deadline) < 0 && !cancelled()) {
    const int available = input.available();
    if (available <= 0) { delay(2); continue; }
    size_t wanted = bytes - received;
    if (wanted > 4096) wanted = 4096;
    if (wanted > static_cast<size_t>(available)) wanted = available;
    const size_t count = input.readBytes(reinterpret_cast<char *>(output + received), wanted);
    if (count > wanted) return received;  // Broken input cannot overflow accounting.
    if (!count) { delay(2); continue; }
    received += count;
    delay(0);
  }
  return received;
}
}  // namespace AudioDownload
