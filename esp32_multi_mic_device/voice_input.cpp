#include "voice_input.h"

#include <ESP_I2S.h>
#include <FFat.h>
#include <FS.h>
#include <SPIFFS.h>
#include <errno.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <esp_ns.h>
#include <esp_vad.h>
#include <freertos/semphr.h>
#include <math.h>
#include <atomic>

#include "robot_controls.h"
#include "voice_capture_settings.h"
#include "ambient_noise.h"
#include "multi_mic_selector.h"
#include "music_resampler.h"
#include "music_echo_capture.h"

namespace VoiceInput {
namespace {

constexpr size_t FRAMES_PER_CHUNK = 256;
constexpr int MIC_GAIN = 5;
constexpr float HIGH_PASS_ALPHA = 0.995f;
constexpr size_t CAPTURE_FRAMES = 160;
constexpr uint32_t CAPTURE_FRAME_MS = 10;
constexpr float CAPTURE_HIGH_PASS_ALPHA = 0.96f;
constexpr uint8_t DEFAULT_PLAYBACK_VOLUME_PERCENT = 35;
// Audio-taper pots can produce very small ADC readings over half their travel.
// Tune these from /volume diagnostics if the actual endpoints differ.
constexpr uint16_t POT_ADC_MIN = 0;
constexpr uint16_t POT_ADC_MAX = 4095;
constexpr float POT_RESPONSE_EXPONENT = 1.0f / 3.0f;
static_assert(POT_ADC_MIN < POT_ADC_MAX && POT_ADC_MAX <= 4095,
              "Invalid potentiometer ADC calibration");

using Stereo32 = MultiMic::Stereo32;

I2SClass audioBus(I2S_NUM_0);
I2SClass thirdMicrophoneBus(I2S_NUM_1);
MultiMic::Selector microphoneSelector;
bool thirdMicrophoneInitialized = false;
uint8_t wakeFilterMic = 1;
bool probeStatsValid = false;
uint16_t probeRms[3] = {}, probePeak[3] = {};
uint8_t probeHealthyMask = 0;
bool storageMounted = false;
bool initialized = false;
uint32_t currentSampleRate = SAMPLE_RATE;
MusicStream::Decimator musicDecimator;
MusicStream::Decimator referenceDecimator;
struct EchoWork {
  alignas(16) int16_t near[512], reference[512], output[512];
  MusicEcho::NativeFrame native[126];
  Stereo32 primary[126], third[126];
};
EchoWork *echoWork = nullptr;  // Task-only scratch lives in PSRAM, never in ISR.
size_t echoFilled = 0, echoOutputAt = 512;
float echoNearPrevious = 0, echoNearFiltered = 0;
float echoRefPrevious = 0, echoRefFiltered = 0;
fs::FS *activeStorage = nullptr;
const char *activeStorageName = "unmounted";
uint8_t fixedPlaybackVolume = DEFAULT_PLAYBACK_VOLUME_PERCENT;
bool useVolumePotentiometer = false;
uint16_t filteredPotReading = 0;
bool potFilterInitialized = false;
SemaphoreHandle_t microphoneMutex = nullptr;
float wakePreviousInput = 0.0f;
float wakePreviousOutput = 0.0f;
void (*serviceCallback)() = nullptr;
bool recordingFinishRequested = false;
int16_t *retainedRecording = nullptr;
size_t retainedRecordingBytes = 0;
AmbientNoise ambientNoise;
std::atomic<uint32_t> ambientState{0};  // ready bit + Q8 RMS, nonblocking UI snapshot
volatile bool ambientTracking = true;
volatile uint32_t ambientIgnoreUntil = 0;
volatile uint32_t ambientUpdatedAt = 0;
float ambientPreviousInput = 0, ambientPreviousOutput = 0;

void serviceAudio() {
  if (serviceCallback) serviceCallback();
}

class MicrophoneLock {
 public:
  explicit MicrophoneLock(TickType_t timeoutTicks)
      : locked_(microphoneMutex &&
                xSemaphoreTake(microphoneMutex, timeoutTicks) == pdTRUE) {}
  ~MicrophoneLock() {
    if (locked_) xSemaphoreGive(microphoneMutex);
  }
  bool locked() const { return locked_; }

 private:
  bool locked_;
};

bool startThirdMicrophone() {
  thirdMicrophoneBus.setPins(HardwarePins::MIC3_BCLK, HardwarePins::MIC3_WS,
                            -1, HardwarePins::MIC3_DATA);
  if (!thirdMicrophoneBus.begin(I2S_MODE_STD, currentSampleRate,
                                I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.printf("[麦克风] MIC3 I2S1 初始化失败: %d\n", thirdMicrophoneBus.lastError());
    thirdMicrophoneBus.end();
    return false;
  }
  thirdMicrophoneInitialized = true;
  return true;
}

bool readStereoExact(I2SClass &bus, Stereo32 *raw, size_t frames, uint32_t timeoutMs) {
  const uint32_t started = millis();
  size_t received = 0;
  while (received < frames) {
    const uint32_t elapsed = millis() - started;
    if (elapsed >= timeoutMs) return false;
    size_t bytes = 0;
    const size_t wanted = (frames - received) * sizeof(Stereo32);
    const esp_err_t result = i2s_channel_read(bus.rxChan(), raw + received, wanted,
                                             &bytes, timeoutMs - elapsed);
    if (result != ESP_OK || bytes == 0 || bytes > wanted ||
        bytes % sizeof(Stereo32) != 0) return false;
    received += bytes / sizeof(Stereo32);
  }
  return true;
}

// Both controllers are read in bounded blocks. Their sample indices are not
// phase aligned; selecting a channel does not implement beamforming.
bool readMicrophoneBlock(Stereo32 *raw, size_t frames, uint32_t timeoutMs,
                        bool selectChannel = true, bool allowSwitch = true) {
  if (frames == 0 || frames > FRAMES_PER_CHUNK) return false;
  Stereo32 third[FRAMES_PER_CHUNK];
  const uint32_t started = millis();
  if (!readStereoExact(audioBus, raw, frames, timeoutMs)) return false;
  if (microphoneSelector.mask() & 4U) {
    if (!thirdMicrophoneInitialized) return false;
    const uint32_t elapsed = millis() - started;
    if (elapsed >= timeoutMs ||
        !readStereoExact(thirdMicrophoneBus, third, frames, timeoutMs - elapsed)) return false;
  }
  if (!selectChannel) return true;
  probeStatsValid = false;
  const uint8_t previousMic = microphoneSelector.selected();
  const uint8_t selected = microphoneSelector.update(
      raw, thirdMicrophoneInitialized ? third : nullptr, frames, allowSwitch, currentSampleRate);
  if (selected != previousMic) {
    ambientNoise.reset();
    ambientState.store(0, std::memory_order_relaxed);
    ambientPreviousInput = ambientPreviousOutput = 0;
    ambientUpdatedAt = 0;
  }
  for (size_t i = 0; i < frames; ++i) {
    if (selected == 2) raw[i].left = raw[i].right;
    else if (selected == 3) raw[i].left = third[i].left;
  }
  return true;
}

uint16_t readFilteredPot() {
  const uint16_t reading = analogRead(PIN_VOLUME_POT);
  if (!potFilterInitialized) {
    filteredPotReading = reading;
    potFilterInitialized = true;
  } else {
    filteredPotReading = (filteredPotReading * 7U + reading + 4U) / 8U;
  }
  // Let the actual zero endpoint reach mute despite IIR rounding residue.
  if (reading <= POT_ADC_MIN && filteredPotReading <= POT_ADC_MIN + 8) filteredPotReading = POT_ADC_MIN;
  return filteredPotReading;
}

float potFraction(uint16_t reading) {
  if (reading <= POT_ADC_MIN) return 0.0f;
  if (reading >= POT_ADC_MAX) return 1.0f;
  return static_cast<float>(reading - POT_ADC_MIN) / (POT_ADC_MAX - POT_ADC_MIN);
}

uint8_t effectivePlaybackVolume() {
  if (!useVolumePotentiometer) return fixedPlaybackVolume;
  const float fraction = potFraction(readFilteredPot());
  return static_cast<uint8_t>(powf(fraction, POT_RESPONSE_EXPONENT) * 100.0f + 0.5f);
}

int16_t saturate16(int32_t sample) {
  if (sample > 32767) return 32767;
  if (sample < -32768) return -32768;
  return static_cast<int16_t>(sample);
}

// A gentle boost at the bottom of the slider: 0 remains true silence, while
// 1..100 stays strictly increasing. Calculate once per chunk, not per sample.
uint16_t playbackGainQ15(uint8_t percent) {
  if (percent == 0) return 0;
  return static_cast<uint16_t>(sqrtf(constrain(percent, 0, 100) / 100.0f) * 32768.0f + 0.5f);
}

uint16_t effectivePlaybackGainQ15() {
  if (!useVolumePotentiometer) return playbackGainQ15(fixedPlaybackVolume);
  // Keep all 12 ADC bits until the final gain. The percentage readout is only
  // for display; rounding it to 0..100 must not silence the lower pot travel.
  const float fraction = potFraction(readFilteredPot());
  return static_cast<uint16_t>(powf(fraction, POT_RESPONSE_EXPONENT) * 32768.0f + 0.5f);
}

size_t writeMono16(const int16_t *mono, size_t samples, uint16_t gainQ15 = 32768) {
  Stereo32 frames[FRAMES_PER_CHUNK];
  size_t totalFrames = 0;
  while (totalFrames < samples) {
    const size_t count = min(FRAMES_PER_CHUNK, samples - totalFrames);
    for (size_t i = 0; i < count; ++i) {
      // Scale directly into the 32-bit I2S slot. Truncating back to int16_t first
      // discards quiet samples; multiplying also avoids shifting negative ints.
      const int32_t value = static_cast<int32_t>(mono[totalFrames + i]) * gainQ15 * 2;
      frames[i].left = value;
      frames[i].right = value;
    }
    const size_t wanted = count * sizeof(Stereo32);
    const size_t written = audioBus.write(frames, wanted);
    if (written != wanted) return totalFrames + min(written / sizeof(Stereo32), count);
    totalFrames += count;
  }
  return totalFrames;
}

void writeSilence(uint16_t durationMs) {
  int16_t silence[FRAMES_PER_CHUNK] = {};
  size_t remaining = (static_cast<size_t>(currentSampleRate) * durationMs) / 1000;
  while (remaining > 0) {
    const size_t count = min(remaining, static_cast<size_t>(FRAMES_PER_CHUNK));
    writeMono16(silence, count);
    remaining -= count;
  }
}

bool discardMicrophoneUnlocked(uint32_t durationMs) {
  Stereo32 raw[FRAMES_PER_CHUNK];
  size_t remaining = static_cast<size_t>(currentSampleRate) * durationMs / 1000U;
  while (remaining > 0) {
    const size_t count = min(remaining, FRAMES_PER_CHUNK);
    if (!readMicrophoneBlock(raw, count, 100, false)) return false;
    remaining -= count;
  }
  wakePreviousInput = 0.0f;
  wakePreviousOutput = 0.0f;
  // Keep the wake listener's selected channel through the prompt/RX drain.
  musicDecimator.reset();
  microphoneSelector.reset(true);
  wakeFilterMic = microphoneSelector.selected();
  return true;
}

class CaptureProcessor {
 public:
  explicit CaptureProcessor(const VoiceCapture::Settings &config) {
    const auto mode = static_cast<vad_mode_t>(3U - config.sensitivity * 3U / 100U);
    vad = vad_create(mode);
    if (config.noiseSuppression) ns = ns_pro_create(CAPTURE_FRAME_MS, 1, SAMPLE_RATE);
    valid = vad && (!config.noiseSuppression || ns);
  }
  ~CaptureProcessor() {
    if (ns) ns_destroy(ns);
    if (vad) vad_destroy(vad);
  }
  ns_handle_t ns = nullptr;
  vad_handle_t vad = nullptr;
  bool valid = false;
};

class CaptureBuffer {
 public:
  explicit CaptureBuffer(size_t samples)
      : data(static_cast<int16_t *>(heap_caps_malloc(
            samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) {}
  ~CaptureBuffer() { free(data); }
  CaptureBuffer(const CaptureBuffer &) = delete;
  CaptureBuffer &operator=(const CaptureBuffer &) = delete;
  int16_t *release() { int16_t *result = data; data = nullptr; return result; }
  int16_t *data;
};

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
  MusicEcho::setMuted(mute);
  digitalWrite(PIN_AMP_SD, mute ? LOW : HIGH);
}

bool begin() {
  if (!mountStorage()) return false;
  if (initialized) return true;

  if (!microphoneMutex) microphoneMutex = xSemaphoreCreateMutex();
  if (!microphoneMutex) {
    Serial.println(F("[语音] 无法创建麦克风互斥锁。"));
    return false;
  }

  pinMode(PIN_AMP_SD, OUTPUT);
  muteAmplifier(true);
  RobotControls::begin();
  pinMode(PIN_VOLUME_POT, INPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_VOLUME_POT, ADC_11db);

  audioBus.setPins(PIN_I2S_BCLK, PIN_I2S_WS, PIN_AMP_DATA, PIN_MIC_DATA);
  currentSampleRate = SAMPLE_RATE;
  if (!audioBus.begin(I2S_MODE_STD, currentSampleRate, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.printf("[语音] I2S 初始化失败: %d\n", audioBus.lastError());
    audioBus.end();
    return false;
  }
  if ((microphoneSelector.mask() & 4U) && !startThirdMicrophone()) {
    audioBus.end();
    return false;
  }

  // Feed zeroes before enabling the class-D amplifier, avoiding start-up pops.
  writeSilence(40);
  wakePreviousInput = 0.0f;
  wakePreviousOutput = 0.0f;
  initialized = true;
  return true;
}

bool ready() {
  return initialized;
}

bool configureMicrophones(uint8_t installedMask, uint8_t preferredMic) {
  if (!MultiMic::validConfiguration(installedMask, preferredMic)) return false;
  if (!microphoneMutex) microphoneMutex = xSemaphoreCreateMutex();
  MicrophoneLock lock(0);
  if (!lock.locked()) return false;
  if (initialized && (installedMask & 4U) && !thirdMicrophoneInitialized &&
      !startThirdMicrophone()) return false;  // Previous configuration remains valid.
  if (!(installedMask & 4U) && thirdMicrophoneInitialized) {
    if (!thirdMicrophoneBus.end()) return false;
    thirdMicrophoneInitialized = false;
  }
  microphoneSelector.configure(installedMask, preferredMic);
  ambientNoise.reset();
  ambientState.store(0, std::memory_order_relaxed);
  ambientPreviousInput = ambientPreviousOutput = 0;
  ambientUpdatedAt = 0;
  probeStatsValid = false;
  wakePreviousInput = wakePreviousOutput = 0.0f;
  wakeFilterMic = microphoneSelector.selected();
  return true;
}

uint8_t microphoneMask() { return microphoneSelector.mask(); }
uint8_t preferredMicrophone() { return microphoneSelector.preferred(); }
uint8_t selectedMicrophone() { return microphoneSelector.selected(); }
uint8_t microphoneProbeMask() { return probeHealthyMask; }

String microphoneDiagnostics() {
  MicrophoneLock lock(0);
  if (!lock.locked()) return String("[麦克风] 采集忙，请稍后查看。");
  String result = String("[麦克风] 安装掩码=") + String(microphoneSelector.mask()) +
      "，模式=" + (microphoneSelector.preferred() ?
      String("固定 MIC") + String(microphoneSelector.preferred()) : String("auto")) +
      "，当前 MIC" + String(microphoneSelector.selected()) +
      "，I2S1=" + (thirdMicrophoneInitialized ? "on" : "off") +
      "，采集块=" + String(microphoneSelector.blocks()) +
      (probeStatsValid ? "，以下为最近诊断段的 RMS/最大峰值" : "，以下为最近采集块（原始增益）");
  for (uint8_t c = 0; c < 3; ++c) {
    result = result + "\nMIC" + String(c + 1) +
        ((microphoneSelector.mask() & (1U << c)) ? " 已启用" : " 未安装/停用") +
        " RMS=" + String(probeStatsValid ? probeRms[c] : microphoneSelector.rms(c)) +
        " peak=" + String(probeStatsValid ? probePeak[c] : microphoneSelector.peak(c)) +
        (probeStatsValid ? ((probeHealthyMask & (1U << c)) ? " 检测到变化信号" : " 未通过信号检测") : "");
  }
  return result;
}

bool probeMicrophones(uint32_t durationMs) {
  probeStatsValid = false;
  probeHealthyMask = 0;
  if (!begin()) return false;
  MicrophoneLock lock(0);
  if (!lock.locked()) return false;
  muteAmplifier(true);
  if (!discardMicrophoneUnlocked(120)) return false;
  Stereo32 raw[CAPTURE_FRAMES];
  size_t remaining = SAMPLE_RATE * min(durationMs, uint32_t{3000}) / 1000U;
  uint64_t squares[3] = {};
  size_t samples = 0;
  for (uint8_t c = 0; c < 3; ++c) probePeak[c] = 0;
  while (remaining > 0) {
    serviceAudio();
    if (RobotControls::abortRequested()) return false;
    const size_t count = min(remaining, CAPTURE_FRAMES);
    if (!readMicrophoneBlock(raw, count, 100)) return false;
    for (uint8_t c = 0; c < 3; ++c) {
      const uint64_t rms = microphoneSelector.rms(c);
      squares[c] += rms * rms * count;
      probePeak[c] = max(probePeak[c], microphoneSelector.peak(c));
    }
    samples += count;
    remaining -= count;
  }
  for (uint8_t c = 0; c < 3; ++c)
    probeRms[c] = samples ? static_cast<uint16_t>(sqrt(static_cast<double>(squares[c]) / samples)) : 0;
  // A successful DMA read does not prove a microphone is connected. Require
  // changing acoustic samples on every enabled channel; silence/DC fail closed.
  for (uint8_t c = 0; c < 3; ++c)
    if (probeRms[c] >= 8 && probePeak[c] < 32760) probeHealthyMask |= 1U << c;
  probeStatsValid = true;
  return (probeHealthyMask & microphoneSelector.mask()) == microphoneSelector.mask();
}

void setServiceCallback(void (*callback)()) { serviceCallback = callback; }

bool discardMicrophone(uint32_t settleMs) {
  if (!initialized) return false;
  MicrophoneLock lock(pdMS_TO_TICKS(1000));
  return lock.locked() && discardMicrophoneUnlocked(min(settleMs, uint32_t{1000}));
}

bool releaseAudioHardware() {
  if (!initialized) return true;
  MicrophoneLock lock(pdMS_TO_TICKS(1000));
  if (!lock.locked()) {
    Serial.println(F("[语音] 麦克风仍在使用，无法安全释放 I2S。"));
    return false;
  }
  muteAmplifier(true);
  bool ok = true;
  if (thirdMicrophoneInitialized) {
    ok = thirdMicrophoneBus.end();
    thirdMicrophoneInitialized = false;
  }
  ok = audioBus.end() && ok;
  MusicEcho::end();
  free(echoWork); echoWork = nullptr;
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
                       bool stopWhenButtonReleased, uint32_t silenceStopMs,
                       AudioStats &stats, bool manual = false, bool keepInMemory = false) {
  clearRecording();
  stats = AudioStats();
  const uint32_t preparationStarted = millis();
  if (currentSampleRate != SAMPLE_RATE) { stats.error = "音乐尚未暂停，不能开始录音"; return false; }
  if (!begin()) {
    stats.error = "音频或存储初始化失败";
    return false;
  }
  MicrophoneLock microphoneLock(portMAX_DELAY);
  if (!microphoneLock.locked()) {
    stats.error = "获取麦克风锁失败";
    return false;
  }
  muteAmplifier(true);

  const VoiceCapture::Settings config = VoiceCapture::settings();
  if (!keepInMemory) activeStorage->remove(path);
  const size_t minimumSamples = (static_cast<size_t>(SAMPLE_RATE) * minimumMs) / 1000;
  const size_t targetSamples = (static_cast<size_t>(SAMPLE_RATE) * maximumMs) / 1000;
  CaptureBuffer captureBuffer(targetSamples);
  if (!captureBuffer.data) {
    stats.error = "PSRAM 录音缓冲分配失败";
    return false;
  }
  CaptureProcessor processor(config);
  if (!processor.valid) {
    stats.error = processor.vad ? "降噪初始化失败" : "语音检测初始化失败";
    Serial.println(F("[语音] 降噪或语音检测初始化失败，本轮不上传。"));
    return false;
  }
  stats.noiseSuppressionApplied = processor.ns != nullptr;
  // The default RX DMA holds about 90 ms, including the preceding prompt tone.
  if (!discardMicrophoneUnlocked(120)) {
    stats.error = "读取麦克风缓冲失败";
    return false;
  }

  File file;
  Stereo32 raw[CAPTURE_FRAMES];
  size_t captured = 0;
  int16_t inputPcm[CAPTURE_FRAMES];
  int16_t pcm[CAPTURE_FRAMES];
  float previousInput = 0.0f;
  float previousOutput = 0.0f;
  uint64_t sumSquares = 0;
  uint32_t peak = 0;
  uint32_t speechRunMs = 0;
  uint32_t speechGapMs = 0;
  size_t lastSpeechSample = 0;
  const size_t silenceStopSamples =
      (static_cast<size_t>(SAMPLE_RATE) * silenceStopMs) / 1000U;
  const size_t initialSpeechTimeoutSamples =
      static_cast<size_t>(SAMPLE_RATE) * config.listenSeconds;
  const uint32_t speechThreshold = config.sensitivity <= 50
      ? 400U - config.sensitivity * 260U / 50U
      : 140U - (config.sensitivity - 50U) * 80U / 50U;
  // Snapshot under the microphone lock: capture never updates its baseline.
  const uint32_t ambientThreshold = millis() - ambientUpdatedAt < 30000
      ? ambientNoise.threshold(speechThreshold, config.gain) : speechThreshold;
  const uint32_t started = millis();
  stats.preparationMs = started - preparationStarted;
  uint32_t previousReadAt = micros();
  bool captureFinished = false;
  uint32_t captureEnded = 0;
  uint32_t storageStarted = 0;
  auto updateStats = [&]() {
    stats.samples = captured;
    stats.durationMs = (captureFinished ? captureEnded : millis()) - started;
    stats.peak = min(peak, static_cast<uint32_t>(65535));
    stats.rms = captured ? static_cast<uint16_t>(sqrt(static_cast<double>(sumSquares) / captured)) : 0;
  };
  auto failRecording = [&](const char *reason) {
    updateStats();
    if (captureFinished && !keepInMemory) stats.storageMs = millis() - storageStarted;
    stats.error = reason;
    file.close();
    if (!keepInMemory) activeStorage->remove(path);
    stats.fileBytes = 0;
    return false;
  };

  while (captured < targetSamples) {
    const uint32_t serviceStarted = micros();
    serviceAudio();
    stats.webServiceUs += micros() - serviceStarted;
    if (RobotControls::abortRequested()) {
      return failRecording("录音已取消");
    }
    if (manual && recordingFinishRequested && captured >= minimumSamples) break;
    const size_t frames = min(CAPTURE_FRAMES, targetSamples - captured);
    const uint32_t readAt = micros();
    stats.maxReadGapUs = max(stats.maxReadGapUs, readAt - previousReadAt);
    previousReadAt = readAt;
    // Freeze the selected microphone for the complete utterance.
    if (!readMicrophoneBlock(raw, frames, 1000, true, false)) return failRecording("I2S 读取失败");
    const uint32_t processingStarted = micros();
    uint64_t chunkSquares = 0;
    uint64_t inputSquares = 0;
    uint32_t chunkPeak = 0;
    for (size_t i = 0; i < frames; ++i) {
      // readMicrophoneBlock has selected MIC1, MIC2 or MIC3 into the left field.
      const float input = static_cast<float>(raw[i].left >> 16);
      const float filtered = input - previousInput + CAPTURE_HIGH_PASS_ALPHA * previousOutput;
      previousInput = input;
      previousOutput = filtered;
      const int32_t amplified = static_cast<int32_t>(filtered * config.gain);
      if (amplified > 32767 || amplified < -32768) ++stats.clippedSamples;
      inputPcm[i] = saturate16(amplified);
      inputSquares += static_cast<int64_t>(inputPcm[i]) * inputPcm[i];
    }
    for (size_t i = frames; i < CAPTURE_FRAMES; ++i) inputPcm[i] = 0;
    if (processor.ns) ns_process(processor.ns, inputPcm, pcm);
    else memcpy(pcm, inputPcm, sizeof(pcm));
    const bool vadSpeech = vad_process(processor.vad, pcm, SAMPLE_RATE,
                                       CAPTURE_FRAME_MS) == VAD_SPEECH;
    for (size_t i = 0; i < frames; ++i) {
      const int16_t sample = pcm[i];
      const uint32_t magnitude = sample < 0 ? -static_cast<int32_t>(sample) : sample;
      peak = max(peak, magnitude);
      sumSquares += static_cast<int64_t>(sample) * sample;
      chunkPeak = max(chunkPeak, magnitude);
      chunkSquares += static_cast<int64_t>(sample) * sample;
    }
    stats.processingUs += micros() - processingStarted;
    // Keep flash writes outside acquisition: slow SPIFFS writes can outlast RX DMA.
    memcpy(captureBuffer.data + captured, pcm, frames * sizeof(int16_t));
    captured += frames;
    const uint32_t chunkRms = static_cast<uint32_t>(
        sqrt(static_cast<double>(chunkSquares) / frames));
    const uint32_t inputRms = static_cast<uint32_t>(sqrt(static_cast<double>(inputSquares) / frames));
    const bool speech = vadSpeech && inputRms >= ambientThreshold && chunkRms >= speechThreshold &&
                        chunkPeak >= speechThreshold * 2U;
    if (speech && frames == CAPTURE_FRAMES) {
      stats.speechMs += CAPTURE_FRAME_MS;
      speechRunMs += CAPTURE_FRAME_MS;
      speechGapMs = 0;
      if (speechRunMs >= 200U) stats.speechDetected = true;
      lastSpeechSample = captured;
    } else {
      speechGapMs += CAPTURE_FRAME_MS;
      if (speechGapMs > 30U) speechRunMs = 0;
    }
    if (silenceStopSamples > 0) {
      if (captured >= minimumSamples &&
          captured - lastSpeechSample >= silenceStopSamples) {
        Serial.println(stats.speechDetected
                           ? F("[语音] 已检测到自然停顿，自动结束录音。")
                           : F("[语音] 持续静音，结束收音，本轮不上传。"));
        break;
      }
      if (!stats.speechDetected && captured >= initialSpeechTimeoutSamples) {
        Serial.println(F("[语音] 等待说话超时，本轮不上传。"));
        break;
      }
    }
    if (RobotControls::abortRequested()) {
      Serial.println(F("[语音] 中断键已按下，取消本次录音。"));
      return failRecording("录音已取消");
    }
    if (stopWhenButtonReleased && captured >= minimumSamples &&
        !RobotControls::talkPressed()) {
      break;
    }
  }

  captureFinished = true;
  captureEnded = millis();
  updateStats();
  if (!stats.speechDetected) return true;

  if (keepInMemory) {
    if (RobotControls::abortRequested()) return failRecording("录音已取消");
    retainedRecordingBytes = captured * sizeof(int16_t);
    retainedRecording = captureBuffer.release();
    stats.fileBytes = retainedRecordingBytes;
    return true;
  }

  storageStarted = millis();
  file = activeStorage->open(path, FILE_WRITE);
  if (!file) return failRecording("无法创建录音文件");
  const size_t totalBytes = captured * sizeof(int16_t);
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(captureBuffer.data);
  size_t saved = 0;
  while (saved < totalBytes) {
    serviceAudio();
    if (RobotControls::abortRequested()) return failRecording("录音已取消");
    const size_t count = min(size_t{8192}, totalBytes - saved);
    errno = 0;
    const size_t written = file.write(bytes + saved, count);
    stats.storageBytesWritten = saved + written;
    if (written != count) {
      stats.storageError = errno;
      return failRecording("写入录音文件失败");
    }
    saved += count;
  }
  if (RobotControls::abortRequested()) return failRecording("录音已取消");
  file.flush();
  stats.fileBytes = file.size();
  file.close();
  stats.storageMs = millis() - storageStarted;
  if (RobotControls::abortRequested()) return failRecording("录音已取消");
  if (stats.fileBytes != totalBytes) return failRecording("录音文件大小校验失败");
  return true;
}

bool recordPcm(const char *path, uint32_t durationMs, AudioStats &stats) {
  return recordPcmInternal(path, durationMs, durationMs, false, 0, stats);
}

bool recordPcmPushToTalk(const char *path, uint32_t minimumMs, uint32_t maximumMs,
                         AudioStats &stats, bool keepInMemory) {
  minimumMs = constrain(minimumMs, 200UL, 3000UL);
  maximumMs = constrain(maximumMs, minimumMs, 60000UL);
  return recordPcmInternal(path, minimumMs, maximumMs, true, 0, stats, false, keepInMemory);
}

bool recordPcmHandsFree(const char *path, uint32_t minimumMs,
                        uint32_t maximumMs, uint32_t silenceStopMs,
                        AudioStats &stats, bool keepInMemory) {
  minimumMs = constrain(minimumMs, 400UL, 5000UL);
  maximumMs = constrain(maximumMs, minimumMs, 60000UL);
  silenceStopMs = constrain(silenceStopMs, 600UL, 4000UL);
  return recordPcmInternal(path, minimumMs, maximumMs, false, silenceStopMs,
                           stats, false, keepInMemory);
}

bool recordPcmManual(const char *path, uint32_t maximumMs, AudioStats &stats, bool keepInMemory) {
  recordingFinishRequested = false;
  maximumMs = constrain(maximumMs, 400UL, 60000UL);
  return recordPcmInternal(path, 400, maximumMs, false, 0, stats, true, keepInMemory);
}

void requestRecordingFinish() { recordingFinishRequested = true; }
uint8_t *recordingData() { return reinterpret_cast<uint8_t *>(retainedRecording); }
size_t recordingBytes() { return retainedRecordingBytes; }
void clearRecording() {
  free(retainedRecording);
  retainedRecording = nullptr;
  retainedRecordingBytes = 0;
}

bool readMicrophoneMono16(int16_t *samples, size_t sampleCount,
                          uint32_t timeoutMs) {
  if (!samples || sampleCount == 0 || !initialized) return false;
  MicrophoneLock lock(pdMS_TO_TICKS(timeoutMs + 20U));
  if (!lock.locked() || !initialized) return false;

  if (currentSampleRate == 48000 && MusicEcho::active() && echoWork) {
    auto &echoNear = echoWork->near;
    auto &echoReference = echoWork->reference;
    auto &echoOutput = echoWork->output;
    size_t completed = 0;
    while (completed < sampleCount) {
      if (echoOutputAt < 512) {
        const float input = echoOutput[echoOutputAt++];
        const float filtered = input - wakePreviousInput + HIGH_PASS_ALPHA * wakePreviousOutput;
        wakePreviousInput = input; wakePreviousOutput = filtered;
        samples[completed++] = saturate16(static_cast<int32_t>(filtered * MIC_GAIN));
        continue;
      }
      auto &native = echoWork->native;
      auto &primary = echoWork->primary;
      auto &third = echoWork->third;
      const size_t count = min((512 - echoFilled) * 3, size_t{126});
      if (!MusicEcho::readNative(native, count, timeoutMs)) return false;
      for (size_t i = 0; i < count; ++i) {
        primary[i] = {int32_t(native[i].mic[0]) * 65536, int32_t(native[i].mic[1]) * 65536};
        third[i] = {int32_t(native[i].mic[2]) * 65536, 0};
      }
      const uint8_t selected = microphoneSelector.update(primary,
          thirdMicrophoneInitialized ? third : nullptr, count, true, 48000);
      if (wakeFilterMic != selected) {
        musicDecimator.reset(); referenceDecimator.reset(); echoFilled = 0;
        wakePreviousInput = wakePreviousOutput = 0;
        echoNearPrevious = echoNearFiltered = echoRefPrevious = echoRefFiltered = 0;
        wakeFilterMic = selected;
      }
      for (size_t i = 0; i < count; ++i) {
        float near = 0, ref = 0;
        const bool nearReady = musicDecimator.push(native[i].mic[selected - 1], near);
        const bool refReady = referenceDecimator.push(native[i].reference, ref);
        if (!nearReady || !refReady) continue;
        const float nearFiltered = near - echoNearPrevious + HIGH_PASS_ALPHA * echoNearFiltered;
        const float refFiltered = ref - echoRefPrevious + HIGH_PASS_ALPHA * echoRefFiltered;
        echoNearPrevious = near; echoNearFiltered = nearFiltered;
        echoRefPrevious = ref; echoRefFiltered = refFiltered;
        echoNear[echoFilled] = saturate16(static_cast<int32_t>(nearFiltered));
        echoReference[echoFilled++] = saturate16(static_cast<int32_t>(refFiltered));
      }
      if (echoFilled == 512) {
        MusicEcho::process(echoNear, echoReference, echoOutput);
        echoFilled = 0; echoOutputAt = 0;
      }
    }
    return true;
  }

  Stereo32 raw[128];
  size_t completed = 0;
  while (completed < sampleCount) {
    const bool resample = currentSampleRate == 48000;
    const size_t wantedFrames = min((sampleCount - completed) * (resample ? 3 : 1),
                                    static_cast<size_t>(resample ? 126 : 128));
    if (!readMicrophoneBlock(raw, wantedFrames, timeoutMs)) return false;
    const size_t frames = wantedFrames;
    if (wakeFilterMic != microphoneSelector.selected()) {
      wakePreviousInput = wakePreviousOutput = 0.0f;
      musicDecimator.reset();
      wakeFilterMic = microphoneSelector.selected();
    }
    double ambientSquares = 0;
    for (size_t i = 0; i < frames; ++i) {
      float input = static_cast<float>(raw[i].left >> 16);
      if (resample && !musicDecimator.push(input, input)) continue;
      const float noiseFiltered = input - ambientPreviousInput + CAPTURE_HIGH_PASS_ALPHA * ambientPreviousOutput;
      ambientPreviousInput = input;
      ambientPreviousOutput = noiseFiltered;
      ambientSquares += noiseFiltered * noiseFiltered;
      const float filtered =
          input - wakePreviousInput + HIGH_PASS_ALPHA * wakePreviousOutput;
      wakePreviousInput = input;
      wakePreviousOutput = filtered;
      samples[completed++] =
          saturate16(static_cast<int32_t>(filtered * MIC_GAIN));
    }
    if (!resample && ambientTracking && static_cast<int32_t>(millis() - ambientIgnoreUntil) >= 0 && frames) {
      ambientNoise.observe(sqrt(ambientSquares / frames));
      ambientUpdatedAt = millis();
      ambientState.store(ambientNoise.ready() ? 0x80000000U | static_cast<uint32_t>(ambientNoise.floor() * 256) : 0,
                         std::memory_order_relaxed);
    }
  }
  return true;
}

void setAmbientTracking(bool enabled) { ambientTracking = enabled; }
void resetAmbientNoise() {
  MicrophoneLock lock(pdMS_TO_TICKS(200));
  if (lock.locked()) { ambientNoise.reset(); ambientState.store(0, std::memory_order_relaxed); ambientPreviousInput = ambientPreviousOutput = 0; }
}
float ambientNoiseFloor() {
  return (ambientState.load(std::memory_order_relaxed) & 0x7fffffffU) / 256.0f;
}
bool ambientNoiseReady() {
  return (ambientState.load(std::memory_order_relaxed) & 0x80000000U) && millis() - ambientUpdatedAt < 30000;
}
uint32_t ambientSpeechThreshold() {
  const auto config = VoiceCapture::settings();
  const uint32_t fixed = config.sensitivity <= 50 ? 400U - config.sensitivity * 260U / 50U
      : 140U - (config.sensitivity - 50U) * 80U / 50U;
  return ambientNoiseReady() ? max(fixed, static_cast<uint32_t>(ambientNoiseFloor() * config.gain * 2.5f + 20)) : fixed;
}
bool playAlarmChunk(const int16_t *samples, size_t count) {
  if (!samples || !count || count > 1600 || !initialized || currentSampleRate != SAMPLE_RATE) return false;
  ambientIgnoreUntil = millis() + 350;
  muteAmplifier(false);
  return writeMono16(samples, count, playbackGainQ15(min(effectivePlaybackVolume(), uint8_t{45}))) == count;
}

uint32_t hardwareSampleRate() { return currentSampleRate; }
bool setMusicRate(bool enabled) {
  const uint32_t wanted = enabled ? 48000 : SAMPLE_RATE;
  if (!initialized) return !enabled && begin();
  MicrophoneLock lock(pdMS_TO_TICKS(600));
  if (!lock.locked()) return false;
  if (wanted == currentSampleRate) return true;
  // Feed bounded silence while the amp is still enabled. This drains accepted
  // song frames from TX DMA before end() discards that queue on a rate change.
  if (currentSampleRate == 48000) writeSilence(80);
  muteAmplifier(true); audioBus.end();
  if (thirdMicrophoneInitialized) { thirdMicrophoneBus.end(); thirdMicrophoneInitialized = false; }
  MusicEcho::end();
  free(echoWork); echoWork = nullptr;
  currentSampleRate = wanted;
  bool ok = audioBus.begin(I2S_MODE_STD, wanted, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  if (ok && (microphoneSelector.mask() & 4U)) ok = startThirdMicrophone();
  if (ok && enabled) {
    echoWork = static_cast<EchoWork *>(heap_caps_aligned_alloc(16, sizeof(EchoWork), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ok = echoWork && MusicEcho::begin(audioBus.txChan(), audioBus.rxChan(),
        thirdMicrophoneInitialized ? thirdMicrophoneBus.rxChan() : nullptr, microphoneSelector.mask());
  }
  if (!ok) {
    audioBus.end(); if (thirdMicrophoneInitialized) thirdMicrophoneBus.end(); thirdMicrophoneInitialized = false;
    MusicEcho::end();
    free(echoWork); echoWork = nullptr;
    currentSampleRate = SAMPLE_RATE;
    initialized = audioBus.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    if (initialized && (microphoneSelector.mask() & 4U)) initialized = startThirdMicrophone();
  }
  musicDecimator.reset(); wakePreviousInput = wakePreviousOutput = 0;
  referenceDecimator.reset(); echoFilled = 0; echoOutputAt = 512;
  echoNearPrevious = echoNearFiltered = echoRefPrevious = echoRefFiltered = 0;
  ambientNoise.reset(); ambientState.store(0, std::memory_order_relaxed); ambientUpdatedAt = 0;
  ambientPreviousInput = ambientPreviousOutput = 0; ambientIgnoreUntil = millis() + 1000;
  return ok;
}
size_t playMusicChunk(const int16_t *samples, size_t count) {
  if (!initialized || currentSampleRate != 48000 || !samples || !count || count > 480) return 0;
  ambientIgnoreUntil = millis() + 350; muteAmplifier(false);
  return writeMono16(samples, count, playbackGainQ15(effectivePlaybackVolume()));
}

bool playPcmFile(const char *path) {
  if (currentSampleRate != SAMPLE_RATE) return false;
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
    const uint16_t gain = effectivePlaybackGainQ15();
    if (samples && writeMono16(pcm, samples, gain) != samples) {
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
    serviceAudio();
    if (buttonCanAbort && RobotControls::abortRequested()) {
      ok = false;
      break;
    }
    const size_t wanted = min(remaining, sizeof(pcm));
    const size_t bytes = input.readBytes(reinterpret_cast<char *>(pcm), wanted);
    if (bytes == 0 || (bytes & 1U) != 0) {
      ok = false;
      break;
    }
    const size_t samples = bytes / sizeof(int16_t);
    const uint16_t gain = effectivePlaybackGainQ15();
    if (writeMono16(pcm, samples, gain) != samples) {
      ok = false;
      break;
    }
    remaining -= bytes;
    if (buttonCanAbort && RobotControls::abortRequested()) {
      Serial.println(F("[播放] 中断键已按下，停止本次回答。"));
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
  ambientIgnoreUntil = millis() + durationMs + 500;
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

String volumeDiagnostics() {
  const uint16_t raw = analogRead(PIN_VOLUME_POT);
  return String("[音量] 模式=") + (useVolumePotentiometer ? "电位器" : "网页固定音量") +
      "，GPIO=" + String(PIN_VOLUME_POT) + "，原始ADC=" + String(raw) +
      "，滤波ADC=" + String(filteredPotReading) + "，固定音量=" + String(fixedPlaybackVolume) +
      "%\n[音量] 旋钮从最小/中间/最大分别执行 /volume；若半圈原始ADC一直为0，请检查电位器滑臂/3V3/GND接线。";
}

}  // namespace VoiceInput
