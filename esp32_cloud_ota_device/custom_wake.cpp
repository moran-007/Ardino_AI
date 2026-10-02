#include "custom_wake.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stddef.h>

#include "offline_wake.h"
#include "voice_input.h"

namespace CustomWake {
namespace {
constexpr size_t FFT_SIZE = 512;
constexpr size_t BANDS = 12;
constexpr size_t MAX_FRAMES = 125;  // 2.5 seconds including the end-pause.
constexpr size_t MIN_VOICED = 12;
constexpr size_t SILENCE_FRAMES = 14;
constexpr uint32_t MAGIC = 0x31574b43;
constexpr char STORE[] = "/custom_wake.bin";
constexpr char TEMP[] = "/custom_wake.tmp";
constexpr char BACKUP[] = "/custom_wake.bak";
constexpr float kPi = 3.14159265358979323846f;
// Spectrum distance is a mean squared error between normalized log spectra.
// Conservative by design; real microphones and room conditions need calibration.
constexpr float MAX_DISTANCE = 0.13f;

struct AcousticTemplate {
  uint16_t frames;
  uint16_t voiced;
  int8_t features[MAX_FRAMES][BANDS];
};
struct Store {
  uint32_t magic;
  uint16_t version;
  uint8_t on;
  uint8_t count;
  char name[64];
  AcousticTemplate samples[MAX_SAMPLES];
  uint32_t checksum;
};
static_assert(sizeof(Store) < 5000, "Custom wake storage must remain bounded");
Store saved = {};
fs::FS *storage = nullptr;
volatile bool queued = false;
volatile bool recording = false;
volatile bool cancelRequested = false;
String errorText;
String trainingState = "idle";
String messageText;
bool initialized = false;
bool cleanupPending = false;

// Shared scratch is safe because enrollment acknowledges the listener pause.
float fftReal[FFT_SIZE], fftImaginary[FFT_SIZE], window[FRAME_SAMPLES];
float dtwPrevious[MAX_FRAMES + 1], dtwCurrent[MAX_FRAMES + 1];
bool windowReady = false;
struct Capture {
  AcousticTemplate sound = {};
  uint16_t trailing = 0;
  uint32_t clipped = 0;
  uint32_t total = 0;
  float noise = 60.0f;
  bool active = false;
  bool overflow = false;
  bool complete = false;
};
Capture live;

uint32_t hash(const Store &value) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
  uint32_t result = 2166136261U;
  for (size_t i = 0; i < offsetof(Store, checksum); ++i)
    result = (result ^ bytes[i]) * 16777619U;
  return result;
}

void defaults() {
  memset(&saved, 0, sizeof(saved));
  saved.magic = MAGIC;
  saved.version = 1;
  strlcpy(saved.name, "小默小默", sizeof(saved.name));
}

bool valid(const Store &value) {
  if (value.magic != MAGIC || value.version != 1 || value.on > 1 ||
      value.count > MAX_SAMPLES || !memchr(value.name, 0, sizeof(value.name)) ||
      !value.name[0] || value.checksum != hash(value) ||
      (value.on && !value.count)) return false;
  for (uint8_t i = 0; i < value.count; ++i) {
    if (value.samples[i].frames < MIN_VOICED ||
        value.samples[i].frames > MAX_FRAMES ||
        value.samples[i].voiced < MIN_VOICED ||
        value.samples[i].voiced > value.samples[i].frames) return false;
  }
  return true;
}

bool readStore(const char *path, Store &out) {
  File file = storage->open(path, FILE_READ);
  if (!file) return false;
  const bool result = file.size() == sizeof(out) &&
      file.readBytes(reinterpret_cast<char *>(&out), sizeof(out)) == sizeof(out);
  file.close();
  return result && valid(out);
}

bool persist(Store &value) {
  if (!storage) { errorText = "本地存储未就绪"; return false; }
  value.checksum = hash(value);
  storage->remove(TEMP);
  File file = storage->open(TEMP, FILE_WRITE);
  if (!file) { errorText = "无法写入自定义唤醒样本"; return false; }
  const size_t bytes = file.write(reinterpret_cast<const uint8_t *>(&value), sizeof(value));
  file.flush();
  const bool complete = bytes == sizeof(value) && file.size() == sizeof(value);
  file.close();
  if (!complete) {
    storage->remove(TEMP);
    errorText = "自定义唤醒样本写入不完整";
    return false;
  }
  if (storage->exists(BACKUP) && !storage->remove(BACKUP)) {
    storage->remove(TEMP);
    errorText = "无法整理自定义唤醒备份";
    return false;
  }
  const bool old = storage->exists(STORE);
  if (old && !storage->rename(STORE, BACKUP)) {
    storage->remove(TEMP);
    errorText = "无法备份旧唤醒样本";
    return false;
  }
  if (!storage->rename(TEMP, STORE)) {
    if (old) storage->rename(BACKUP, STORE);
    errorText = "无法保存唤醒样本；旧样本保留";
    return false;
  }
  saved = value;
  initialized = true;
  if (storage->exists(BACKUP) && !storage->remove(BACKUP)) {
    cleanupPending = true;
    errorText = "新设置已保存，但旧唤醒样本备份未删除，请重试清空";
    return false;
  }
  cleanupPending = false;
  errorText = "";
  return true;
}

bool pause(bool &restore) {
  restore = !OfflineWake::paused();
  OfflineWake::suspend();
  if (!OfflineWake::paused()) {
    errorText = "麦克风监听尚未暂停，请重试";
    return false;
  }
  return true;
}

void finishMutation(bool restore) {
  resetDetection();
  if (restore) OfflineWake::resume();
}

Store *candidate() {
  auto *result = static_cast<Store *>(malloc(sizeof(Store)));
  if (!result) errorText = "自定义唤醒存储内存不足";
  else *result = saved;
  return result;
}

bool mutationAllowed() {
  if (!initialized) { errorText = "自定义唤醒存储未初始化"; return false; }
  if (queued || recording) { errorText = "正在录制样本，请先取消或等待完成"; return false; }
  return true;
}

float features(const int16_t *pcm, int8_t *out, uint32_t &clipped) {
  if (!windowReady) {
    for (size_t i = 0; i < FRAME_SAMPLES; ++i)
      window[i] = 0.5f - 0.5f * cosf(2 * kPi * i / (FRAME_SAMPLES - 1));
    windowReady = true;
  }
  float energy = 0, mean = 0;
  for (size_t i = 0; i < FRAME_SAMPLES; ++i) mean += pcm[i];
  mean /= FRAME_SAMPLES;
  for (size_t i = 0; i < FFT_SIZE; ++i) {
    const float value = i < FRAME_SAMPLES ? pcm[i] - mean : 0;
    if (i < FRAME_SAMPLES) {
      energy += value * value;
      if (pcm[i] >= 32700 || pcm[i] <= -32700) ++clipped;
    }
    fftReal[i] = i < FRAME_SAMPLES ? value * window[i] : 0;
    fftImaginary[i] = 0;
  }
  // In-place radix-2 FFT, 512 points. Twelve triangular mel filters are stable
  // across gain changes; per-frame log-spectrum normalization removes volume.
  for (size_t i = 1, j = 0; i < FFT_SIZE; ++i) {
    size_t bit = FFT_SIZE >> 1;
    while (j & bit) { j ^= bit; bit >>= 1; }
    j ^= bit;
    if (i < j) {
      const float r = fftReal[i]; fftReal[i] = fftReal[j]; fftReal[j] = r;
      const float im = fftImaginary[i]; fftImaginary[i] = fftImaginary[j]; fftImaginary[j] = im;
    }
  }
  for (size_t width = 2; width <= FFT_SIZE; width <<= 1) {
    const float stepR = cosf(-2 * kPi / width), stepI = sinf(-2 * kPi / width);
    for (size_t start = 0; start < FFT_SIZE; start += width) {
      float wr = 1, wi = 0;
      for (size_t offset = 0; offset < width / 2; ++offset) {
        const size_t a = start + offset, b = a + width / 2;
        const float r = wr * fftReal[b] - wi * fftImaginary[b];
        const float im = wr * fftImaginary[b] + wi * fftReal[b];
        fftReal[b] = fftReal[a] - r; fftImaginary[b] = fftImaginary[a] - im;
        fftReal[a] += r; fftImaginary[a] += im;
        const float next = wr * stepR - wi * stepI;
        wi = wr * stepI + wi * stepR; wr = next;
      }
    }
  }
  float bins[BANDS], average = 0;
  constexpr float lowMel = 180.0f;
  const float highMel = 2595.0f * log10f(1.0f + 6500.0f / 700.0f);
  const float step = (highMel - lowMel) / (BANDS + 1);
  for (size_t band = 0; band < BANDS; ++band) {
    const float left = 700.0f * (powf(10.0f, (lowMel + band * step) / 2595.0f) - 1);
    const float center = 700.0f * (powf(10.0f, (lowMel + (band + 1) * step) / 2595.0f) - 1);
    const float right = 700.0f * (powf(10.0f, (lowMel + (band + 2) * step) / 2595.0f) - 1);
    float power = 0;
    for (size_t bin = 1; bin < FFT_SIZE / 2; ++bin) {
      const float hz = static_cast<float>(bin) * VoiceInput::SAMPLE_RATE / FFT_SIZE;
      if (hz < left || hz > right) continue;
      const float weight = hz < center ? (hz - left) / (center - left) : (right - hz) / (right - center);
      power += weight * (fftReal[bin] * fftReal[bin] + fftImaginary[bin] * fftImaginary[bin]);
    }
    bins[band] = logf(1.0f + power);
    average += bins[band];
  }
  average /= BANDS;
  float norm = 0;
  for (size_t band = 0; band < BANDS; ++band) {
    bins[band] -= average;
    norm += bins[band] * bins[band];
  }
  norm = sqrtf(norm / BANDS);
  for (size_t band = 0; band < BANDS; ++band) {
    const int value = static_cast<int>(bins[band] * 32 / max(norm, 0.001f));
    out[band] = static_cast<int8_t>(min(127, max(-127, value)));
  }
  return sqrtf(energy / FRAME_SAMPLES);
}

void addFrame(Capture &capture, const int16_t *pcm) {
  if (capture.complete) return;
  int8_t spectrum[BANDS];
  uint32_t clipped = 0;
  const float rms = features(pcm, spectrum, clipped);
  const float threshold = max(180.0f, capture.noise * 2.5f + 70.0f);
  const bool voiced = rms >= threshold;
  if (!capture.active && !voiced) {
    // Learn background only while idle; speech does not raise its own threshold.
    capture.noise = 0.96f * capture.noise + 0.04f * min(rms, 250.0f);
    return;
  }
  capture.active = true;
  capture.clipped += clipped;
  capture.total += FRAME_SAMPLES;
  if (capture.sound.frames < MAX_FRAMES) {
    memcpy(capture.sound.features[capture.sound.frames++], spectrum, BANDS);
  } else capture.overflow = true;
  if (voiced) { ++capture.sound.voiced; capture.trailing = 0; }
  else if (++capture.trailing >= SILENCE_FRAMES) capture.complete = true;
}

bool finalize(Capture &capture) {
  if (!capture.active || capture.overflow || capture.sound.voiced < MIN_VOICED ||
      capture.clipped * 100U > capture.total) return false;
  if (capture.trailing <= capture.sound.frames)
    capture.sound.frames -= capture.trailing;
  return capture.sound.frames >= MIN_VOICED;
}

float distance(const AcousticTemplate &a, const AcousticTemplate &b) {
  // Duration guard and a 25% warping band reject arbitrary fragments/reordering.
  if (a.frames * 100U < b.frames * 65U || a.frames * 100U > b.frames * 150U)
    return 100.0f;
  constexpr float FAR = 1e9f;
  for (size_t j = 0; j <= b.frames; ++j) dtwPrevious[j] = FAR;
  dtwPrevious[0] = 0;
  const int band = max(8, max(static_cast<int>(a.frames), static_cast<int>(b.frames)) / 4);
  for (size_t i = 1; i <= a.frames; ++i) {
    for (size_t j = 0; j <= b.frames; ++j) dtwCurrent[j] = FAR;
    for (size_t j = 1; j <= b.frames; ++j) {
      if (abs(static_cast<int>(i) - static_cast<int>(j)) > band) continue;
      float cost = 0;
      for (size_t k = 0; k < BANDS; ++k) {
        const float delta = (a.features[i - 1][k] - b.features[j - 1][k]) / 32.0f;
        cost += delta * delta;
      }
      cost /= BANDS;
      dtwCurrent[j] = cost + min(dtwPrevious[j - 1], min(dtwPrevious[j], dtwCurrent[j - 1]));
    }
    memcpy(dtwPrevious, dtwCurrent, sizeof(float) * (b.frames + 1));
  }
  return dtwPrevious[b.frames] / max(a.frames, b.frames);
}

bool failTraining(const char *error) {
  errorText = error;
  messageText = error;
  trainingState = cancelRequested ? "cancelled" : "error";
  return false;
}
}  // namespace

bool begin(fs::FS &store) {
  storage = &store;
  defaults();
  queued = recording = cancelRequested = false;
  trainingState = "idle";
  messageText = "填写唤醒词后逐次录制，建议 3 份样本";
  errorText = "";
  initialized = false;
  cleanupPending = storage->exists(BACKUP);
  Store *loaded = candidate();
  if (!loaded) { initialized = false; return false; }
  if (storage->exists(STORE)) {
    // An existing primary is authoritative, even when damaged. Falling back
    // here could resurrect biometric templates after an explicit reset.
    if (readStore(STORE, *loaded)) {
      saved = *loaded;
      initialized = true;
      if (cleanupPending) messageText = "旧唤醒样本备份仍存在，请重试清空以彻底删除";
    } else {
      errorText = "自定义唤醒样本损坏，已停用；请先清空样本后重新录制";
      messageText = errorText;
    }
  } else if (storage->exists(BACKUP)) {
    // A missing primary is the one recoverable interrupted-rotation state.
    if (readStore(BACKUP, *loaded) && storage->rename(BACKUP, STORE)) {
      saved = *loaded;
      initialized = true;
      cleanupPending = false;
      messageText = "已恢复中断保存前的唤醒样本";
    } else {
      errorText = "无法恢复唤醒样本，已停用；请先清空样本后重新录制";
      messageText = errorText;
    }
  } else {
    initialized = true;
  }
  free(loaded);
  resetDetection();
  return errorText.isEmpty();
}

bool setLabel(const String &input) {
  if (!mutationAllowed()) return false;
  String name = input; name.trim();
  if (name.isEmpty() || name.length() > 60) {
    errorText = "唤醒词需为 1 至 60 字节，建议 3 至 6 个字"; return false;
  }
  for (size_t i = 0; i < name.length(); ++i)
    if (static_cast<uint8_t>(name[i]) < 32) { errorText = "唤醒词不能包含控制字符"; return false; }
  if (name == saved.name) { errorText = ""; return true; }
  bool restore = false;
  if (!pause(restore)) return false;
  Store *next = candidate();
  bool result = false;
  if (next) {
    next->on = 0; next->count = 0;
    memset(next->samples, 0, sizeof(next->samples));
    memset(next->name, 0, sizeof(next->name));
    strlcpy(next->name, name.c_str(), sizeof(next->name));
    result = persist(*next); free(next);
  }
  if (result) { trainingState = "idle"; messageText = "唤醒词已保存，请录制新样本"; }
  finishMutation(restore);
  return result;
}

bool enable(bool on) {
  if (!mutationAllowed()) return false;
  if (on && !ready()) { errorText = "请先成功录制至少 1 份样本，建议 3 份"; return false; }
  bool restore = false;
  if (!pause(restore)) return false;
  Store *next = candidate();
  const bool result = next && (next->on = on, persist(*next));
  free(next);
  if (result) messageText = on ? "实验自定义唤醒已开启" : "自定义唤醒已关闭";
  finishMutation(restore);
  return result;
}

bool reset() {
  // Explicit deletion also repairs an unreadable primary. No other mutation
  // can enable or train a store that failed closed during initialization.
  if (!storage) { errorText = "自定义唤醒存储未初始化"; return false; }
  if (queued || recording) { errorText = "正在录制样本，请先取消或等待完成"; return false; }
  bool restore = false;
  if (!pause(restore)) return false;
  Store *next = candidate();
  bool result = false;
  if (next) {
    next->on = 0; next->count = 0; memset(next->samples, 0, sizeof(next->samples));
    result = persist(*next); free(next);
  }
  if (result) { trainingState = "idle"; messageText = "样本已清空，自定义唤醒已停用"; }
  finishMutation(restore);
  return result;
}

bool queueTraining() {
  if (!mutationAllowed()) return false;
  if (saved.count >= MAX_SAMPLES) { errorText = "已录制 3 份样本；重新训练请先清空"; return false; }
  if (!VoiceInput::ready()) { errorText = "麦克风未就绪"; return false; }
  cancelRequested = false; queued = true;
  trainingState = "queued"; messageText = "已排队；设备空闲时录制，请关注录音状态";
  errorText = "";
  return true;
}

void cancelTraining() {
  cancelRequested = true;
  queued = false;
  if (!recording) { trainingState = "cancelled"; messageText = "样本录制已取消，已有样本保留"; }
}

bool processQueuedTraining(void (*service)()) {
  if (!queued || recording) return false;
  return recordSample(service);
}

bool recordSample(void (*service)()) {
  if (!initialized || recording || saved.count >= MAX_SAMPLES)
    return failTraining("无法开始样本录制");
  cancelRequested = false;
  queued = false;
  bool restore = false;
  if (!pause(restore)) return failTraining("麦克风监听暂停失败，未开始录制");
  recording = true;
  trainingState = "recording"; messageText = "正在录音：请说 1 次唤醒词，3 秒后完成";
  bool result = false;
  if (!VoiceInput::ready() || !VoiceInput::discardMicrophone(120)) {
    failTraining("麦克风未就绪或准备失败");
  } else {
    Capture capture;
    int16_t pcm[FRAME_SAMPLES];
    size_t frames = 0;
    bool readFailed = false;
    const uint32_t started = millis();
    while (frames < 150 && millis() - started < 5000 && !cancelRequested) {
      if (service) service();
      if (cancelRequested) break;
      if (!VoiceInput::readMicrophoneMono16(pcm, FRAME_SAMPLES, 120)) { readFailed = true; break; }
      addFrame(capture, pcm);
      ++frames;
      yield();
    }
    if (cancelRequested) failTraining("样本录制已取消，已有样本保留");
    else if (readFailed || frames < 150) failTraining("麦克风读取超时，请重试");
    else if (capture.clipped * 100U > capture.total && capture.total)
      failTraining("录音削波过多，请降低麦克风增益或远离麦克风");
    else if (capture.overflow) failTraining("唤醒词过长，请在 0.3 至 2 秒内说完并停顿");
    else if (!capture.complete || !finalize(capture)) failTraining("没有完整唤醒词，请在安静环境清晰说 1 次并停顿，至少 0.3 秒");
    else {
      Store *next = candidate();
      if (next) {
        next->samples[next->count++] = capture.sound;
        result = persist(*next); free(next);
      }
      if (result) {
        trainingState = "success";
        messageText = "样本已保存在本机（" + String(saved.count) + "/3）；录满后手动开启";
      } else { trainingState = "error"; messageText = errorText; }
    }
  }
  recording = false;
  finishMutation(restore);
  return result;
}

bool process(const int16_t *pcm, size_t count) {
  if (!enabled() || recording || !pcm || count != FRAME_SAMPLES) return false;
  addFrame(live, pcm);
  if (!live.complete) return false;
  bool detected = false;
  if (finalize(live))
    for (uint8_t i = 0; i < saved.count; ++i)
      if (distance(live.sound, saved.samples[i]) <= MAX_DISTANCE) { detected = true; break; }
  resetDetection();
  return detected;
}

void resetDetection() { live = Capture(); }
bool enabled() { return initialized && saved.on && ready(); }
bool ready() { return initialized && saved.count > 0; }
bool trainingPending() { return queued; }
bool trainingActive() { return recording; }
uint8_t sampleCount() { return saved.count; }
const char *label() { return saved.name; }
const char *lastError() { return errorText.c_str(); }

String toJson() {
  JsonDocument doc;
  doc["label"] = saved.name;
  doc["initialized"] = initialized;
  doc["cleanupPending"] = cleanupPending;
  doc["enabled"] = enabled();
  doc["ready"] = ready();
  doc["samples"] = saved.count;
  doc["recommendedSamples"] = MAX_SAMPLES;
  doc["training"] = trainingState.c_str();
  doc["message"] = errorText.isEmpty() ? messageText.c_str() : errorText.c_str();
  doc["experimental"] = true;
  doc["storage"] = "device";
  doc["rawAudioStored"] = false;
  char json[768]; serializeJson(doc, json, sizeof(json)); return String(json);
}
}  // namespace CustomWake
