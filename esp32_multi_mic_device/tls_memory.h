#pragma once

namespace TlsMemory {
// Configure once at startup, before creating any HTTPS clients. The normal
// internal allocator remains in use when PSRAM is unavailable.
bool begin();
}
