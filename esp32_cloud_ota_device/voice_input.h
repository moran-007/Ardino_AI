#pragma once

#include <Arduino.h>
#include <FS.h>

#include "hardware_pins.h"

namespace VoiceInput {

// INMP441 and MAX98357A share BCLK/WS but keep independent data lines.
constexpr int PIN_I2S_BCLK = HardwarePins::I2S_BCLK;
constexpr int PIN_I2S_WS = HardwarePins::I2S_WS;
constexpr int PIN_MIC_DATA = HardwarePins::MIC_DATA;
constexpr int PIN_AMP_DATA = HardwarePins::AMP_DATA;
constexpr int PIN_AMP_SD = HardwarePins::AMP_SD;
constexpr int PIN_VOLUME_POT = HardwarePins::SHARED_ADC;
constexpr uint32_t SAMPLE_RATE = 16000;

struct AudioStats {
  uint32_t samples = 0;
  uint32_t durationMs = 0;
  uint16_t peak = 0;
  uint16_t rms = 0;
  size_t fileBytes = 0;  // Uploadable PCM bytes, including the memory-only path.
  bool speechDetected = false;
  uint32_t speechMs = 0;
  uint32_t clippedSamples = 0;
  bool noiseSuppressionApplied = false;
  uint32_t preparationMs = 0;
  uint32_t webServiceUs = 0;
  uint32_t processingUs = 0;
  uint32_t maxReadGapUs = 0;
  uint32_t storageMs = 0;
  size_t storageBytesWritten = 0;
  int storageError = 0;
  const char *error = "";
};

bool begin();
bool ready();
bool releaseAudioHardware();
bool discardMicrophone(uint32_t settleMs = 120);
void setServiceCallback(void (*callback)());
fs::FS &recordingStorage();
const char *recordingStorageName();
bool recordPcm(const char *path, uint32_t durationMs, AudioStats &stats);
bool recordPcmPushToTalk(const char *path, uint32_t minimumMs, uint32_t maximumMs,
                         AudioStats &stats, bool keepInMemory = false);
bool recordPcmHandsFree(const char *path, uint32_t minimumMs, uint32_t maximumMs,
                        uint32_t silenceStopMs, AudioStats &stats, bool keepInMemory = false);
bool recordPcmManual(const char *path, uint32_t maximumMs, AudioStats &stats,
                     bool keepInMemory = false);
// Borrowed PCM stays valid until clearRecording() or the next capture.
uint8_t *recordingData();
size_t recordingBytes();
void clearRecording();
void requestRecordingFinish();
bool readMicrophoneMono16(int16_t *samples, size_t sampleCount,
                          uint32_t timeoutMs = 100);
void setAmbientTracking(bool enabled);
void resetAmbientNoise();
float ambientNoiseFloor();
bool ambientNoiseReady();
uint32_t ambientSpeechThreshold();
// Bounded music chunk: RX remains available to the wake listener during TX.
bool playAlarmChunk(const int16_t *samples, size_t count);
bool playPcmFile(const char *path);
bool playPcmStream(Stream &input, size_t byteCount, bool buttonCanAbort = true);
bool playTone(uint16_t frequencyHz = 880, uint16_t durationMs = 140, uint8_t volumePercent = 8);
void muteAmplifier(bool mute);
void setPlaybackVolume(uint8_t percent);
uint8_t playbackVolume();
void usePotentiometerVolume(bool enabled);
bool potentiometerVolumeEnabled();
uint8_t readPotentiometerVolume();
String volumeDiagnostics();

}  // namespace VoiceInput
