#include "music_echo_capture.h"
#include <esp_aec.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>

namespace MusicEcho {
namespace {
// Arduino's I2S driver uses 240-frame DMA blocks. Keep 60 ms of paired audio
// in internal SRAM so ISR capture is also safe during flash cache stalls.
constexpr size_t BLOCK = 240, CAPACITY = BLOCK * 12;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
int16_t *queue = nullptr;
uint8_t installedMask = 1, stride = 2;
size_t head = 0, tail = 0, used = 0;
int16_t referenceBlock[BLOCK] = {}, thirdBlock[BLOCK] = {};
uint32_t referenceAt = 0, thirdAt = 0;
bool running = false, muted = true;
aec_handle_t *aec = nullptr;
Diagnostics stats;
const char *errorText = "";

bool IRAM_ATTR sent(i2s_chan_handle_t, i2s_event_data_t *event, void *) {
  if (!event || !event->dma_buf || event->size != BLOCK * 8) return false;
  const auto *data = static_cast<const int32_t *>(event->dma_buf);
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL_ISR(&mux);
  for (size_t i = 0; i < BLOCK; ++i) referenceBlock[i] = muted ? 0 : data[i * 2] >> 16;
  referenceAt = now;
  portEXIT_CRITICAL_ISR(&mux);
  return false;
}
bool IRAM_ATTR receivedThird(i2s_chan_handle_t, i2s_event_data_t *event, void *) {
  if (!event || !event->dma_buf || event->size != BLOCK * 8) return false;
  const auto *data = static_cast<const int32_t *>(event->dma_buf);
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL_ISR(&mux);
  for (size_t i = 0; i < BLOCK; ++i) thirdBlock[i] = data[i * 2] >> 16;
  thirdAt = now;
  portEXIT_CRITICAL_ISR(&mux);
  return false;
}
bool IRAM_ATTR received(i2s_chan_handle_t, i2s_event_data_t *event, void *) {
  if (!event || !event->dma_buf || event->size != BLOCK * 8) return false;
  const auto *data = static_cast<const int32_t *>(event->dma_buf);
  const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
  portENTER_CRITICAL_ISR(&mux);
  if (running && queue) {
    // Ref completed on the same clock, at most one DMA block before this RX.
    // A stalled/missing reference must become zero, never repeat old music.
    const bool validRef = referenceAt && uint32_t(now - referenceAt) <= 10000;
    const bool validThird = thirdAt && uint32_t(now - thirdAt) <= 10000;
    if (!validRef) stats.missingReferenceFrames += BLOCK;
    if (CAPACITY - used < BLOCK) {
      // Drop a whole oldest block; keep mic/ref indices inseparable.
      head = (head + BLOCK) % CAPACITY; used -= BLOCK;
      stats.droppedFrames += BLOCK;
    }
    for (size_t i = 0; i < BLOCK; ++i) {
      int16_t *frame = queue + tail * stride;
      size_t channel = 0;
      if (installedMask & 1) frame[channel++] = data[i * 2] >> 16;
      if (installedMask & 2) frame[channel++] = data[i * 2 + 1] >> 16;
      if (installedMask & 4) frame[channel++] = validThird ? thirdBlock[i] : 0;
      frame[channel] = validRef ? referenceBlock[i] : 0;
      tail = (tail + 1) % CAPACITY;
    }
    used += BLOCK;
  }
  portEXIT_CRITICAL_ISR(&mux);
  return false;
}
bool install(i2s_chan_handle_t channel, const i2s_event_callbacks_t &callbacks) {
  if (!channel || i2s_channel_disable(channel) != ESP_OK) return false;
  const bool ok = i2s_channel_register_event_callback(channel, &callbacks, nullptr) == ESP_OK;
  const bool enabled = i2s_channel_enable(channel) == ESP_OK;
  return ok && enabled;
}
uint16_t rms(const int16_t *samples, size_t count) {
  double squares = 0;
  for (size_t i = 0; i < count; ++i) squares += double(samples[i]) * samples[i];
  return count ? sqrt(squares / count) : 0;
}
}

bool begin(i2s_chan_handle_t tx, i2s_chan_handle_t rx, i2s_chan_handle_t third, uint8_t mask) {
  errorText = "";
  if (!MultiMic::validConfiguration(mask, 0) || ((mask & 4) && !third)) {
    errorText = "回声输入麦克风配置无效"; return false;
  }
  installedMask = mask;
  stride = 1 + ((mask & 1) != 0) + ((mask & 2) != 0) + ((mask & 4) != 0);
  queue = static_cast<int16_t *>(heap_caps_calloc(CAPACITY * stride, sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!queue) { errorText = "回声参考缓冲区内存不足"; return false; }
  aec_config_t config = {};
  config.mic_num = config.ref_num = config.out_num = 1;
  config.filter_length = 4; config.sample_rate = 16000;
  config.caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  config.mode = AEC_MODE_SR_HIGH_PERF; config.nlp_level = AEC_NLP_LEVEL_NORMAL;
  aec = aec_create_from_config(&config);
  if (!aec || aec_get_chunksize(aec) != 512) {
    errorText = "回声消除初始化失败";
    if (aec) aec_destroy(aec);
    aec = nullptr; free(queue); queue = nullptr; return false;
  }
  portENTER_CRITICAL(&mux);
  head = tail = used = 0; referenceAt = thirdAt = 0; stats = Diagnostics{};
  portEXIT_CRITICAL(&mux);
  i2s_event_callbacks_t txCallbacks = {}, rxCallbacks = {}, thirdCallbacks = {};
  txCallbacks.on_sent = sent; rxCallbacks.on_recv = received; thirdCallbacks.on_recv = receivedThird;
  const bool ok = install(tx, txCallbacks) && install(rx, rxCallbacks) && (!third || install(third, thirdCallbacks));
  portENTER_CRITICAL(&mux); running = ok; stats.active = ok; portEXIT_CRITICAL(&mux);
  if (!ok) errorText = "回声参考 DMA 注册失败";
  // On failure callbacks may already exist; keep storage until channel deletion.
  return ok;
}
void end() {
  portENTER_CRITICAL(&mux); running = false; stats.active = false; portEXIT_CRITICAL(&mux);
  if (aec) aec_destroy(aec);
  aec = nullptr; free(queue); queue = nullptr; head = tail = used = 0;
}
void setMuted(bool value) { portENTER_CRITICAL(&mux); muted = value; portEXIT_CRITICAL(&mux); }
bool active() { portENTER_CRITICAL(&mux); const bool value = running; portEXIT_CRITICAL(&mux); return value; }
bool readNative(NativeFrame *frames, size_t count, uint32_t timeoutMs) {
  if (!frames || !count || count > BLOCK) return false;
  const uint32_t started = millis();
  for (;;) {
    portENTER_CRITICAL(&mux);
    const bool ok = running && used >= count;
    if (ok) {
      for (size_t i = 0; i < count; ++i) {
        const int16_t *frame = queue + head * stride;
        size_t channel = 0;
        for (uint8_t mic = 0; mic < 3; ++mic) frames[i].mic[mic] = (installedMask & (1 << mic)) ? frame[channel++] : 0;
        frames[i].reference = frame[channel]; head = (head + 1) % CAPACITY;
      }
      used -= count;
    }
    portEXIT_CRITICAL(&mux);
    if (ok) return true;
    if (!active() || millis() - started >= timeoutMs) return false;
    delay(1);
  }
}
size_t frameSize() { return aec ? aec_get_chunksize(aec) : 0; }
void process(int16_t *near, int16_t *reference, int16_t *out) {
  if (!aec || !near || !reference || !out) return;
  const uint32_t started = micros();
  aec_process(aec, near, reference, out);
  const uint32_t elapsed = micros() - started;
  const uint16_t inRms = rms(near, 512), refRms = rms(reference, 512), outRms = rms(out, 512);
  portENTER_CRITICAL(&mux);
  stats.frames++; stats.inputRms = inRms; stats.referenceRms = refRms; stats.outputRms = outRms;
  if (elapsed > stats.processMaxUs) stats.processMaxUs = elapsed;
  portEXIT_CRITICAL(&mux);
}
Diagnostics diagnostics() { portENTER_CRITICAL(&mux); const auto result = stats; portEXIT_CRITICAL(&mux); return result; }
const char *error() { return errorText; }
}
