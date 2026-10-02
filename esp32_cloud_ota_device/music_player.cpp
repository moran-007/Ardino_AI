#include "music_player.h"
#include "voice_input.h"
#include <esp_heap_caps.h>

namespace MusicPlayer {
namespace {
int16_t *pcm = nullptr;
size_t bytes = 0, position = 0;
bool complete = false;
String title;
}
uint8_t *prepare(size_t count) {
  stop();
  if (count < 3200 || count > MAX_BYTES || count % 2) return nullptr;
  pcm = static_cast<int16_t *>(heap_caps_malloc(count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (pcm) bytes = count;
  return reinterpret_cast<uint8_t *>(pcm);
}
void loaded(const String &name) { complete = pcm != nullptr; title = name; }
bool available() { return complete; }
bool tick() {
  if (!complete) return false;
  const size_t count = min(size_t{1600}, (bytes - position) / 2);
  const bool ok = VoiceInput::playAlarmChunk(pcm + position / 2, count);
  if (!ok) { stop(); return false; }
  position += count * 2;
  if (position == bytes) position = 0;
  return true;
}
void pause() { VoiceInput::muteAmplifier(true); }
void stop() { pause(); free(pcm); pcm = nullptr; bytes = position = 0; complete = false; title = ""; }
String status() { return complete ? title : String("本机提示音"); }
}
