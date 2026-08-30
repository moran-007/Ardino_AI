#pragma once

#include <Arduino.h>
#include <FS.h>

namespace VoiceInput {

// One shared I2S bus. INMP441 and MAX98357A share BCLK/WS but have separate
// data lines. ESP32-S3 has no general GPIO22-25, so use low, commonly exposed
// pins that do not collide with native USB (GPIO19/20) or common flash/PSRAM.
#if CONFIG_IDF_TARGET_ESP32S3
constexpr int PIN_I2S_BCLK = 5;
constexpr int PIN_I2S_WS = 4;
constexpr int PIN_MIC_DATA = 6;
constexpr int PIN_AMP_DATA = 7;
constexpr int PIN_AMP_SD = 15;
constexpr int PIN_VOLUME_POT = 1;
#else
constexpr int PIN_I2S_BCLK = 26;
constexpr int PIN_I2S_WS = 25;
constexpr int PIN_MIC_DATA = 33;
constexpr int PIN_AMP_DATA = 22;
constexpr int PIN_AMP_SD = 27;
constexpr int PIN_VOLUME_POT = 34;
#endif
constexpr int PIN_VOICE_BUTTON = 0;  // On-board BOOT button, active LOW.
constexpr uint32_t SAMPLE_RATE = 16000;

struct AudioStats {
  uint32_t samples = 0;
  uint32_t durationMs = 0;
  uint16_t peak = 0;
  uint16_t rms = 0;
  size_t fileBytes = 0;
};

bool begin();
bool ready();
bool releaseAudioHardware();
fs::FS &recordingStorage();
const char *recordingStorageName();
bool recordPcm(const char *path, uint32_t durationMs, AudioStats &stats);
bool recordPcmPushToTalk(const char *path, uint32_t minimumMs, uint32_t maximumMs,
                         AudioStats &stats);
bool playPcmFile(const char *path);
bool playPcmStream(Stream &input, size_t byteCount, bool buttonCanAbort = true);
bool playTone(uint16_t frequencyHz = 880, uint16_t durationMs = 140, uint8_t volumePercent = 8);
void muteAmplifier(bool mute);
void setPlaybackVolume(uint8_t percent);
uint8_t playbackVolume();
void usePotentiometerVolume(bool enabled);
bool potentiometerVolumeEnabled();
uint8_t readPotentiometerVolume();

}  // namespace VoiceInput
