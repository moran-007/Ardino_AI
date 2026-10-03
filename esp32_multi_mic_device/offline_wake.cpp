#include "offline_wake.h"

#include <ESP_SR.h>  // Registers the Arduino ESP-SR library/model build hook.
#include <esp_partition.h>
#include <esp_wn_iface.h>
#include <model_path.h>

#include "voice_input.h"
#include "custom_wake.h"

// Exported by ESP-SR's libwakenet (Arduino-ESP32 3.3.11 / 3.3.12).
// The bundled models all use WakeNet9. The generic name dispatcher also links
// WakeNet10 and its ESP-DL operators in 3.3.12, exceeding our 3 MiB OTA slot.
extern "C" const esp_wn_iface_t esp_sr_wakenet9_quantized;

namespace OfflineWake {
namespace {

// Keep the labels and model IDs together. Only these models are packed into
// wake_models/srmodels.bin, so the web UI cannot select a missing model.
constexpr ModelOption MODEL_OPTIONS[] = {
    {"wn9_nihaoxiaozhi_tts", "你好小智"},
    {"wn9_xiaoaitongxue", "小爱同学"},
    {"wn9_nihaoxiaoxin_tts", "你好小鑫"},
    {"wn9_xiaomeitongxue_tts", "小美同学"},
    {"wn9_xiaolongxiaolong_tts", "小龙小龙"},
    {"wn9_miaomiaotongxue_tts", "喵喵同学"},
    {"wn9_himiaomiao_tts", "Hi 喵喵"},
    {"wn9_hilili_tts", "Hi 莉莉"},
    {"wn9_xiaoyutongxue_tts2", "小宇同学"},
    {"wn9_xiaomingtongxue_tts2", "小明同学"},
    {"wn9_xiaoluxiaolu_tts2", "小鹿小鹿"},
    {"wn9_nihaoxiaoan_tts2", "你好小安"},
    {"wn9_hai1xiao3ou1_tts3", "嗨小欧"},
};

constexpr bool usesOnlyWakeNet9() {
  for (const auto &option : MODEL_OPTIONS) {
    if (option.id[0] != 'w' || option.id[1] != 'n' ||
        option.id[2] != '9' || option.id[3] != '_') return false;
  }
  return true;
}
static_assert(usesOnlyWakeNet9(),
              "Update the WakeNet interface before adding another model family");

srmodel_list_t *models = nullptr;
const esp_wn_iface_t *wakeInterface = nullptr;
model_iface_data_t *wakeModel = nullptr;
TaskHandle_t listenerTaskHandle = nullptr;
volatile bool requestedEnabled = false;
volatile bool suspended = true;
volatile bool pauseAcknowledged = true;
volatile bool detectionPending = false;
volatile bool currentlyListening = false;
const ModelOption *selectedOption = &MODEL_OPTIONS[0];
String errorText;

const ModelOption *findOption(const String &id) {
  for (const auto &option : MODEL_OPTIONS) {
    if (id == option.id) return &option;
  }
  return nullptr;
}

char *findExactModel(const char *id) {
  if (!models || !id) return nullptr;
  for (int index = 0; index < models->num; ++index) {
    if (models->model_name[index] &&
        strcmp(models->model_name[index], id) == 0) {
      return models->model_name[index];
    }
  }
  return nullptr;
}

bool waitUntilPaused(uint32_t timeoutMs = 1000) {
  const uint32_t started = millis();
  while (!pauseAcknowledged && millis() - started < timeoutMs) delay(2);
  return pauseAcknowledged;
}

void destroyActiveModel() {
  if (wakeInterface && wakeModel) wakeInterface->destroy(wakeModel);
  wakeModel = nullptr;
  wakeInterface = nullptr;
}

bool createActiveModel(const ModelOption &option) {
  char *modelName = findExactModel(option.id);
  if (!modelName) {
    errorText = "模型分区中缺少 " + String(option.id);
    return false;
  }
  wakeInterface = &esp_sr_wakenet9_quantized;
  wakeModel = wakeInterface->create(modelName, DET_MODE_90);
  if (!wakeModel) {
    wakeInterface = nullptr;
    errorText = "WakeNet 模型初始化失败 " + String(option.id);
    return false;
  }
  const int rate = wakeInterface->get_samp_rate(wakeModel);
  const int channels = wakeInterface->get_channel_num(wakeModel);
  if (rate != static_cast<int>(VoiceInput::SAMPLE_RATE) || channels != 1) {
    errorText = "WakeNet 音频格式不兼容";
    destroyActiveModel();
    return false;
  }
  selectedOption = &option;
  errorText = "";
  Serial.printf("[离线唤醒] 模型已载入：%s（%s），chunk=%d。\n",
                option.label, option.id,
                wakeInterface->get_samp_chunksize(wakeModel));
  return true;
}

void listenerTask(void *) {
  int16_t *samples = nullptr;
  size_t allocatedSamples = 0;
  for (;;) {
    const bool custom = CustomWake::enabled();
    if ((!requestedEnabled && !custom) || suspended || (!custom && !wakeModel) ||
        !VoiceInput::ready()) {
      currentlyListening = false;
      pauseAcknowledged = true;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    pauseAcknowledged = false;
    // Close the race where foreground suspension follows the loop guard but
    // precedes this acknowledgement. A paused capture must never race an I2S read.
    if (suspended) {
      pauseAcknowledged = true;
      continue;
    }
    const size_t chunkSamples = custom ? CustomWake::FRAME_SAMPLES :
        static_cast<size_t>(wakeInterface->get_samp_chunksize(wakeModel));
    if (chunkSamples == 0 || chunkSamples > 2048) {
      errorText = "WakeNet chunk 长度无效";
      suspended = true;
      continue;
    }
    if (allocatedSamples != chunkSamples) {
      free(samples);
      samples = static_cast<int16_t *>(
          heap_caps_malloc(chunkSamples * sizeof(int16_t),
                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
      allocatedSamples = samples ? chunkSamples : 0;
      if (!samples) {
        errorText = "离线唤醒音频缓冲区内存不足";
        suspended = true;
        continue;
      }
    }

    currentlyListening = true;
    if (!VoiceInput::readMicrophoneMono16(samples, chunkSamples, 120)) {
      currentlyListening = false;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    currentlyListening = false;
    // A foreground enrollment/mutation may have requested pause during the read.
    // Acknowledge on the next loop before it can touch the templates or I2S.
    if (suspended) continue;
    const bool detected = custom ? CustomWake::process(samples, chunkSamples) :
        wakeInterface->detect(wakeModel, samples) == WAKENET_DETECTED;
    // Channel verification is not a second wake-word detection.
    if (detected) {
      detectionPending = true;
      suspended = true;
      pauseAcknowledged = true;
      Serial.printf("[离线唤醒] 已识别：%s。\n", wakeWord());
    }
    taskYIELD();
  }
}

}  // namespace

bool begin(bool enabledValue, const String &modelIdValue) {
  if (listenerTaskHandle) return configure(enabledValue, modelIdValue);
  const ModelOption *option = findOption(modelIdValue);
  if (!option) option = &MODEL_OPTIONS[0];
  selectedOption = option;
  const bool modelLoaded = [&]() {
#if !CONFIG_IDF_TARGET_ESP32S3 || !CONFIG_MODEL_IN_FLASH
  (void)enabledValue;
  (void)modelIdValue;
  errorText = "当前编译目标不支持 ESP-SR Flash 模型";
  return false;
#else
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "model");
  if (!partition) {
    errorText = "未找到 model 分区；Arduino IDE 必须选择 ESP SR 16M 后用 USB 上传";
    Serial.printf("[离线唤醒] %s。\n", errorText.c_str());
    return false;
  }
  uint32_t packedModelCount = 0;
  if (esp_partition_read(partition, 0, &packedModelCount,
                         sizeof(packedModelCount)) != ESP_OK ||
      packedModelCount == 0 || packedModelCount == UINT32_MAX) {
    errorText = "model 分区为空；请把 Partition Scheme 改为 ESP SR 16M 后重新 USB 上传";
    Serial.printf("[离线唤醒] %s。\n", errorText.c_str());
    return false;
  }
  if (packedModelCount != modelOptionCount()) {
    errorText = "model 分区内容不匹配：检测到 " + String(packedModelCount) +
                " 个模型，应为 " + String(modelOptionCount()) + " 个";
    Serial.printf("[离线唤醒] %s。\n", errorText.c_str());
    return false;
  }
  models = esp_srmodel_init("model");
  if (!models || models->num <= 0) {
    errorText = "model 分区为空或模型包无效";
    Serial.printf("[离线唤醒] %s。\n", errorText.c_str());
    return false;
  }
  if (!createActiveModel(*option)) {
    Serial.printf("[离线唤醒] %s。\n", errorText.c_str());
    return false;
  }
  return true;
#endif
  }();
  // Keep a listener available for acoustic enrollment even if the official
  // model partition is absent. Its model error remains visible until usable.
  requestedEnabled = enabledValue;
  suspended = !enabled();
  pauseAcknowledged = true;
  if (xTaskCreatePinnedToCore(listenerTask, "Offline Wake", 6144, nullptr, 3,
                              &listenerTaskHandle, 0) != pdPASS) {
    listenerTaskHandle = nullptr;
    errorText = "无法创建离线唤醒任务";
    destroyActiveModel();
    return false;
  }
  Serial.printf("[离线唤醒] %s，唤醒词：%s。\n",
                enabledValue ? "已开启" : "已关闭", selectedOption->label);
  return modelLoaded || CustomWake::enabled();
}

bool configure(bool enabledValue, const String &modelIdValue) {
  const ModelOption *option = findOption(modelIdValue);
  if (!option) {
    errorText = "不支持的唤醒模型";
    return false;
  }
  requestedEnabled = false;
  suspended = true;
  if (listenerTaskHandle && !waitUntilPaused()) {
    errorText = "离线唤醒任务暂停超时";
    return false;
  }
  if (!listenerTaskHandle) return begin(enabledValue, modelIdValue);
  if (!models) {
    requestedEnabled = enabledValue;
    selectedOption = option;
    detectionPending = false;
    if (CustomWake::enabled()) resume();
    return CustomWake::enabled();
  }
  if (selectedOption != option || !wakeModel) {
    destroyActiveModel();
    if (!createActiveModel(*option)) {
      requestedEnabled = enabledValue;
      if (CustomWake::enabled()) resume();
      return CustomWake::enabled();
    }
  }
  detectionPending = false;
  requestedEnabled = enabledValue;
  if (enabled()) resume();
  Serial.printf("[离线唤醒] 配置已生效：%s，%s。\n",
                enabledValue ? "开启" : "关闭", selectedOption->label);
  return true;
}

void suspend() {
  suspended = true;
  waitUntilPaused();
}

bool paused() { return suspended && pauseAcknowledged && !currentlyListening; }

void resume() {
  suspended = true;
  if (listenerTaskHandle && !waitUntilPaused()) {
    errorText = "离线唤醒任务暂停超时";
    return;
  }
  detectionPending = false;
  CustomWake::resetDetection();
  if ((requestedEnabled && wakeModel) || CustomWake::enabled()) {
    // This SDK's WakeNet9 clean() faults on an internal queue after detection.
    // Keep the model alive; only discard captured playback audio before resuming.
    if (!VoiceInput::ready() || !VoiceInput::discardMicrophone(240)) {
      errorText = "清理麦克风缓冲失败";
      return;
    }
    errorText = "";
    suspended = false;
  }
}

bool consumeDetected() {
  if (!detectionPending) return false;
  detectionPending = false;
  return true;
}

bool ready() { return listenerTaskHandle && (wakeModel || CustomWake::enabled()); }
bool enabled() { return requestedEnabled || CustomWake::enabled(); }
bool listening() {
  return ready() && enabled() && !suspended && currentlyListening;
}
bool listenerActive() { return ready() && enabled() && !suspended; }
const char *modelId() { return selectedOption->id; }
const char *wakeWord() { return CustomWake::enabled() ? CustomWake::label() : selectedOption->label; }
const char *lastError() { return errorText.c_str(); }
size_t modelOptionCount() {
  return sizeof(MODEL_OPTIONS) / sizeof(MODEL_OPTIONS[0]);
}
const ModelOption &modelOption(size_t index) {
  if (index >= modelOptionCount()) index = 0;
  return MODEL_OPTIONS[index];
}
bool isSupportedModel(const String &modelIdValue) {
  return findOption(modelIdValue) != nullptr;
}

}  // namespace OfflineWake
