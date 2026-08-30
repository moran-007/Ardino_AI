#include "voice_input.h"

#include <ESP_I2S.h>
#include <FFat.h>
#include <FS.h>
#include <SPIFFS.h>
#include <esp_partition.h>
#include <math.h>

namespace VoiceInput {
namespace {

constexpr size_t FRAMES_PER_CHUNK = 256;
constexpr int MIC_GAIN = 5;
constexpr float HIGH_PASS_ALPHA = 0.995f;
constexpr uint8_t DEFAULT_PLAYBACK_VOLUME_PERCENT = 35;

struct Stereo32 {
  int32_t left;
  int32_t right;
};

I2SClass audioBus(I2S_NUM_0);
bool storageMounted = false;
bool initialized = false;
fs::FS *activeStorage = nullptr;
const char *activeStorageName = "unmounted";
uint8_t fixedPlaybackVolume = DEFAULT_PLAYBACK_VOLUME_PERCENT;
bool useVolumePotentiometer = false;
uint16_t filteredPotReading = 0;
bool potFilterInitialized = false;

uint8_t effectivePlaybackVolume() {
  if (!useVolumePotentiometer) return fixedPlaybackVolume;
  const uint16_t reading = analogRead(PIN_VOLUME_POT);
  if (!potFilterInitialized) {
    filteredPotReading = reading;
    potFilterInitialized = true;
  } else {
    filteredPotReading = (filteredPotReading * 7U + reading) / 8U;
  }
  return static_cast<uint8_t>((static_cast<uint32_t>(filteredPotReading) * 100U + 2047U) / 4095U);
}

int16_t saturate16(int32_t sample) {
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return static_cast<int16_t>(sample);
}

size_t writeMono16(const int16_t *mono, size_t samples) {
  Stereo32 frames[FRAMES_PER_CHUNK];
  size_t totalFrames = 0;
  while (totalFrames < samples) {
    const size_t count = min(FRAMES_PER_CHUNK, samples - totalFrames);
    for (size_t i = 0; i < count; ++i) {
      const int32_t value = static_cast<int32_t>(mono[totalFrames + i]) << 16;
      frames[i].left = value;
      frames[i].right = value;
    }
    const size_t wanted = count * sizeof(Stereo32);
    const size_t written = audioBus.write(frames, wanted);
    if (written != wanted) return totalFrames;
    totalFrames += count;
  }
  return totalFrames;
}

void writeSilence(uint16_t durationMs) {
  int16_t silence[FRAMES_PER_CHUNK] = {};
  size_t remaining = (static_cast<size_t>(SAMPLE_RATE) * durationMs) / 1000;
  while (remaining > 0) {
    const size_t count = min(remaining, static_cast<size_t>(FRAMES_PER_CHUNK));
    writeMono16(silence, count);
    remaining -= count;
  }
}

}  // namespace

bool mountStorage() {
  if (storageMounted) return true;

  const esp_partition_t *spiffsPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  const esp_partition_t *fatPartition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  if (spiffsPartition) {
    if (!SPIFFS.begin(true)) {
      Serial.println(F("[语音] 找到 SPIFFS 分区，但挂载失败。"));
      return false;
    }
    activeStorage = &SPIFFS;
    activeStorageName = "SPIFFS";
  } else if (fatPartition) {
    if (!FFat.begin(true)) {
      Serial.println(F("[语音] 找到 FATFS 分区，但 FFat 挂载失败。"));
      return false;
    }
    activeStorage = &FFat;
    activeStorageName = "FFat";
  } else {
    Serial.println(F("[语音] 分区表中既没有 SPIFFS，也没有 FATFS。请改用带文件系统的分区方案。"));
    return false;
  }
  storageMounted = true;
  Serial.printf("[语音] 录音临时存储已挂载：%s。\n", activeStorageName);
  return true;
}

void muteAmplifier(bool mute) {
  digitalWrite(PIN_AMP_SD, mute ? LOW : HIGH);
}

bool begin() {
  if (!mountStorage()) return false;
  if (initialized) return true;

  pinMode(PIN_AMP_SD, OUTPUT);
  muteAmplifier(true);
  pinMode(PIN_VOICE_BUTTON, INPUT_PULLUP);
  pinMode(PIN_VOLUME_POT, INPUT);
  analogReadResolution(12);

  audioBus.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DATA, PIN_MIC_DATA);
  if (!audioBus.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.printf("[语音] I2S 初始化失败: %d\n", audioBus.lastError());
    return false;
  }

  // Feed zeroes before enabling the class-D amplifier, avoiding start-up pops.
  writeSilence(40);
  initialized = true;
  return true;
}

bool ready() {
  return initialized;
}

bool releaseAudioHardware() {
  if (!initialized) return true;
  muteAmplifier(true);
  const bool ok = audioBus.end();
  initialized = false;
  return ok;
}

fs::FS &recordingStorage() {
  return *activeStorage;
}

const char *recordingStorageName() {
  return activeStorageName;
}

bool recordPcmInternal(const char *path, uint32_t minimumMs, uint32_t maximumMs,
                       bool stopWhenButtonReleased, AudioStats &stats) {
  stats = AudioStats();
  if (!begin()) return false;
  muteAmplifier(true);

  File file = activeStorage->open(path, FILE_WRITE);
  if (!file) {
    Serial.println(F("[语音] 无法创建录音文件。"));
    return false;
  }

  // Discard stale DMA samples after mode changes.
  Stereo32 raw[FRAMES_PER_CHUNK];
  size_t bytesRead = 0;
  i2s_channel_read(audioBus.rxChan(), raw, sizeof(raw), &bytesRead, 100);

  const size_t minimumSamples = (static_cast<size_t>(SAMPLE_RATE) * minimumMs) / 1000;
  const size_t targetSamples = (static_cast<size_t>(SAMPLE_RATE) * maximumMs) / 1000;
  size_t captured = 0;
  int16_t pcm[FRAMES_PER_CHUNK];
  float previousInput = 0.0f;
  float previousOutput = 0.0f;
  uint64_t sumSquares = 0;
  uint32_t peak = 0;
  const uint32_t started = millis();

  while (captured < targetSamples) {
    bytesRead = 0;
    esp_err_t result = i2s_channel_read(audioBus.rxChan(), raw, sizeof(raw), &bytesRead, 1000);
    if (result != ESP_OK || bytesRead == 0) {
      file.close();
      Serial.printf("[语音] I2S 读取失败: %d\n", static_cast<int>(result));
      return false;
    }

    size_t frames = bytesRead / sizeof(Stereo32);
    frames = min(frames, targetSamples - captured);
    for (size_t i = 0; i < frames; ++i) {
      // INMP441 is 24-bit, MSB-aligned in a 32-bit I2S slot. L/R tied to GND
      // selects the left slot. Apply high-pass filtering and conservative gain.
      const float input = static_cast<float>(raw[i].left >> 16);
      const float filtered = input - previousInput + HIGH_PASS_ALPHA * previousOutput;
      previousInput = input;
      previousOutput = filtered;
      const int16_t sample = saturate16(static_cast<int32_t>(filtered * MIC_GAIN));
      pcm[i] = sample;
      const uint32_t magnitude = sample < 0 ? -static_cast<int32_t>(sample) : sample;
      peak = max(peak, magnitude);
      sumSquares += static_cast<int64_t>(sample) * sample;
    }

    const size_t outputBytes = frames * sizeof(int16_t);
    if (file.write(reinterpret_cast<const uint8_t *>(pcm), outputBytes) != outputBytes) {
      file.close();
      Serial.println(F("[语音] 写入录音文件失败。"));
      return false;
    }
    captured += frames;
    if (stopWhenButtonReleased && captured >= minimumSamples &&
        digitalRead(PIN_VOICE_BUTTON) == HIGH) {
      break;
    }
  }

  file.flush();
  stats.fileBytes = file.size();
  file.close();
  stats.samples = captured;
  stats.durationMs = millis() - started;
  stats.peak = min(peak, static_cast<uint32_t>(65535));
  stats.rms = captured ? static_cast<uint16_t>(sqrt(static_cast<double>(sumSquares) / captured)) : 0;
  return true;
}

bool recordPcm(const char *path, uint32_t durationMs, AudioStats &stats) {
  return recordPcmInternal(path, durationMs, durationMs, false, stats);
}

bool recordPcmPushToTalk(const char *path, uint32_t minimumMs, uint32_t maximumMs,
                         AudioStats &stats) {
  minimumMs = constrain(minimumMs, 200UL, 3000UL);
  maximumMs = constrain(maximumMs, minimumMs, 60000UL);
  return recordPcmInternal(path, minimumMs, maximumMs, true, stats);
}

bool playPcmFile(const char *path) {
  if (!begin()) return false;
  File file = activeStorage->open(path, FILE_READ);
  if (!file) {
    Serial.println(F("[语音] 无法打开合成语音文件。"));
    return false;
  }

  int16_t pcm[FRAMES_PER_CHUNK];
  writeSilence(25);
  muteAmplifier(false);
  delay(10);
  bool ok = true;

  while (file.available()) {
    const size_t bytes = file.read(reinterpret_cast<uint8_t *>(pcm), sizeof(pcm));
    const size_t samples = bytes / sizeof(int16_t);
    const uint8_t volumePercent = effectivePlaybackVolume();
    for (size_t i = 0; i < samples; ++i) {
      pcm[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * volumePercent) / 100);
    }
    if (samples && writeMono16(pcm, samples) != samples) {
      ok = false;
      break;
    }
  }

  writeSilence(50);
  delay(5);
  muteAmplifier(true);
  file.close();
  return ok;
}

bool playPcmStream(Stream &input, size_t byteCount, bool buttonCanAbort) {
  if (!begin() || byteCount == 0 || (byteCount & 1U) != 0) return false;

  int16_t pcm[FRAMES_PER_CHUNK];
  size_t remaining = byteCount;
  writeSilence(25);
  muteAmplifier(false);
  delay(10);
  bool ok = true;

  while (remaining > 0) {
    const size_t wanted = min(remaining, sizeof(pcm));
    const size_t bytes = input.readBytes(reinterpret_cast<char *>(pcm), wanted);
    if (bytes == 0 || (bytes & 1U) != 0) {
      ok = false;
      break;
    }
    const size_t samples = bytes / sizeof(int16_t);
    const uint8_t volumePercent = effectivePlaybackVolume();
    for (size_t i = 0; i < samples; ++i) {
      pcm[i] = static_cast<int16_t>((static_cast<int32_t>(pcm[i]) * volumePercent) / 100);
    }
    if (writeMono16(pcm, samples) != samples) {
      ok = false;
      break;
    }
    remaining -= bytes;
    if (buttonCanAbort && digitalRead(PIN_VOICE_BUTTON) == LOW) {
      Serial.println(F("[播放] BOOT 已按下，停止本次回答。"));
      ok = true;
      break;
    }
    delay(0);
  }

  writeSilence(50);
  delay(5);
  muteAmplifier(true);
  return ok;
}

bool playTone(uint16_t frequencyHz, uint16_t durationMs, uint8_t volumePercent) {
  if (!begin()) return false;
  volumePercent = constrain(volumePercent, 1, 25);  // Protect speaker and ears.
  const int32_t amplitude = (32767L * volumePercent) / 100;
  const size_t totalSamples = (static_cast<size_t>(SAMPLE_RATE) * durationMs) / 1000;
  int16_t pcm[FRAMES_PER_CHUNK];
  size_t generated = 0;

  writeSilence(25);
  muteAmplifier(false);
  delay(10);
  while (generated < totalSamples) {
    const size_t count = min(FRAMES_PER_CHUNK, totalSamples - generated);
    for (size_t i = 0; i < count; ++i) {
      const float phase = 2.0f * PI * frequencyHz * (generated + i) / SAMPLE_RATE;
      pcm[i] = static_cast<int16_t>(sinf(phase) * amplitude);
    }
    if (writeMono16(pcm, count) != count) {
      muteAmplifier(true);
      return false;
    }
    generated += count;
  }
  writeSilence(45);
  delay(5);
  muteAmplifier(true);
  return true;
}

void setPlaybackVolume(uint8_t percent) {
  fixedPlaybackVolume = constrain(percent, 0, 100);
}

uint8_t playbackVolume() {
  return fixedPlaybackVolume;
}

void usePotentiometerVolume(bool enabled) {
  useVolumePotentiometer = enabled;
  potFilterInitialized = false;
}

bool potentiometerVolumeEnabled() {
  return useVolumePotentiometer;
}

uint8_t readPotentiometerVolume() {
  return effectivePlaybackVolume();
}

}  // namespace VoiceInput
