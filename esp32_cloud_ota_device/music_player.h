#pragma once
#include <Arduino.h>
// Alarm clips share the 16 kHz I2S clock with WakeNet. Playback advances in
// 100 ms steps instead of holding the dialogue playback loop for a whole song.
namespace MusicPlayer {
constexpr size_t MAX_BYTES = 1024 * 1024;
uint8_t *prepare(size_t bytes);
void loaded(const String &title);
bool available();
bool tick();
void pause();
void stop();
String status();
}
