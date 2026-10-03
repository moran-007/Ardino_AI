#include "fake_music_echo.h"
#include <cassert>
namespace MusicEcho {
bool begin(i2s_chan_handle_t, i2s_chan_handle_t, i2s_chan_handle_t, uint8_t) {
  FakeEcho::running=FakeEcho::enabled && !FakeEcho::failBegin;
  return !FakeEcho::failBegin;
}
void end() { FakeEcho::running=false; }
void setMuted(bool) {}
bool active() { return FakeEcho::running; }
bool readNative(NativeFrame *out, size_t count, uint32_t) {
  if (FakeEcho::failRead || !FakeEcho::source) return false;
  for (size_t i=0;i<count;++i) out[i]=FakeEcho::source(FakeEcho::nativeFrames++);
  return true;
}
size_t frameSize() { return 512; }
void process(int16_t *near, int16_t *reference, int16_t *out) {
  assert(!(reinterpret_cast<uintptr_t>(near) % 16));
  assert(!(reinterpret_cast<uintptr_t>(reference) % 16));
  assert(!(reinterpret_cast<uintptr_t>(out) % 16));
  for(int i=0;i<512;++i) out[i]=near[i]-reference[i]/4;
  ++FakeEcho::processCalls;
}
Diagnostics diagnostics() { return {}; }
const char *error() { return ""; }
}
