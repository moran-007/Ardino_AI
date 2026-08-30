#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <HTTPClient.h>
#if __has_include(<NetworkClientSecure.h>)
#include <NetworkClientSecure.h>
using SecureClient = NetworkClientSecure;
#else
#include <WiFiClientSecure.h>
using SecureClient = WiFiClientSecure;
#endif
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <time.h>

#include "voice_input.h"
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef CLOUD_PROVISIONED_DEVICE_ID
#define CLOUD_PROVISIONED_DEVICE_ID ""
#endif
#ifndef CLOUD_PROVISIONED_DEVICE_TOKEN
#define CLOUD_PROVISIONED_DEVICE_TOKEN ""
#endif

namespace CloudDevice {

constexpr uint32_t SERIAL_BAUD = 460800;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t RECORD_MINIMUM_MS = 400;
constexpr uint32_t RECORD_MAXIMUM_MS = 20000;
constexpr uint32_t JOB_TIMEOUT_MS = 120000;
constexpr uint32_t NTP_TIMEOUT_MS = 10000;
constexpr size_t MAX_BLE_PAYLOAD = 768;
constexpr size_t RUNTIME_EVENT_COUNT = 24;
constexpr size_t RUNTIME_EVENT_LENGTH = 120;
constexpr size_t MAX_CONVERSATION_TEXT_BYTES = 1400;
constexpr size_t MAX_CONVERSATION_LOG_BYTES = 24576;
constexpr char CONVERSATION_LOG_PATH[] = "/conversation_history.jsonl";
constexpr char DEFAULT_SERVER_URL[] = "https://voice.bsnlch.xyz";
constexpr bool DIALOGUE_TRANSPORT_HTTP = true;
constexpr char RECORDING_PATH[] = "/cloud_question.pcm";
constexpr char BLE_SERVICE_UUID[] = "7b210001-4184-4ea4-a359-856aee830000";
constexpr char BLE_CONFIG_UUID[] = "7b210002-4184-4ea4-a359-856aee830000";
constexpr char BLE_COMMAND_UUID[] = "7b210003-4184-4ea4-a359-856aee830000";
constexpr char BLE_STATUS_UUID[] = "7b210004-4184-4ea4-a359-856aee830000";

// DigiCert Global Root G2, valid until 2038-01-15. The server can renew its
// short-lived leaf certificate without a firmware update while it stays on
// this CA chain. Source: https://www.digicert.com/secure/download-certificate/sc8r65py6v9y5vgy1p0tptkyjkvbw1p
static const char DIGICERT_GLOBAL_ROOT_G2[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)CERT";

enum class DeviceState : uint8_t { Booting, Idle, Recording, Processing, Playing, Error };
enum class BleRequestType : uint8_t { Config, Command };

struct BleRequest {
  BleRequestType type;
  char payload[MAX_BLE_PAYLOAD + 1];
};

struct RuntimeEvent {
  uint32_t atMs;
  char text[RUNTIME_EVENT_LENGTH];
};

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String serverUrl = DEFAULT_SERVER_URL;
  String deviceId;
  String deviceToken;
  uint8_t volumePercent = 50;
  uint8_t chineseSpeakerId = 0;
  uint16_t englishSpeakerId = 0;
  float speechSpeed = 1.0f;
  bool volumePotentiometer = false;
};

Settings settings;
Preferences preferences;
BLECharacteristic *statusCharacteristic = nullptr;
QueueHandle_t bleQueue = nullptr;
bool bleConnected = false;
bool busy = false;
bool audioReady = false;
bool webStarted = false;
bool pendingWifiReconnect = false;
bool pendingVoiceSync = false;
WebServer phoneWeb(80);
SecureClient cloudSecureClient;
WiFiClient cloudPlainClient;
HTTPClient cloudHttp;
String activeServerUrl;
String cloudConnectionBaseUrl;
bool previousButton = HIGH;
String serialLine;
bool clockReady = false;
uint32_t lastCloudActivityMs = 0;
String bleDeviceName;
uint32_t blePin = 0;
RuntimeEvent runtimeEvents[RUNTIME_EVENT_COUNT]{};
size_t runtimeEventHead = 0;
size_t runtimeEventSize = 0;
String dialogueStage = "booting";
String lastDialogueError;
String currentJobId;
int lastHttpCode = 0;
int currentSegmentIndex = -1;
uint32_t dialogueStartedMs = 0;
uint32_t stageStartedMs = 0;
String currentUserText;
String currentAssistantText;
String historyAssistantText;

void traceRuntime(const char *stage, const String &message = "", bool error = false) {
  dialogueStage = stage;
  stageStartedMs = millis();
  if (error) lastDialogueError = message;
  String line = String(stage);
  if (!message.isEmpty()) line += ": " + message;
  RuntimeEvent &event = runtimeEvents[runtimeEventHead];
  event.atMs = millis();
  strlcpy(event.text, line.c_str(), sizeof(event.text));
  runtimeEventHead = (runtimeEventHead + 1) % RUNTIME_EVENT_COUNT;
  runtimeEventSize = min(runtimeEventSize + 1, RUNTIME_EVENT_COUNT);
  Serial.printf("[FLOW][%s] %s\n", stage, message.c_str());
}

void setState(DeviceState state) {
#if defined(RGB_BUILTIN)
  switch (state) {
    case DeviceState::Booting: rgbLedWrite(RGB_BUILTIN, 10, 0, 18); break;
    case DeviceState::Idle: rgbLedWrite(RGB_BUILTIN, 0, 0, 12); break;
    case DeviceState::Recording: rgbLedWrite(RGB_BUILTIN, 28, 0, 0); break;
    case DeviceState::Processing: rgbLedWrite(RGB_BUILTIN, 18, 10, 0); break;
    case DeviceState::Playing: rgbLedWrite(RGB_BUILTIN, 0, 22, 0); break;
    case DeviceState::Error: rgbLedWrite(RGB_BUILTIN, 20, 0, 12); break;
  }
#else
  (void)state;
#endif
}

String hardwareDeviceId() {
  char value[32];
  snprintf(value, sizeof(value), "esp32-s3-%08lX", static_cast<unsigned long>(ESP.getEfuseMac()));
  return value;
}

String normalizedUrl(String value) {
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

String utf8Clip(String value, size_t maximumBytes) {
  if (value.length() <= maximumBytes) return value;
  size_t end = maximumBytes;
  while (end > 0 && (static_cast<uint8_t>(value[end]) & 0xC0) == 0x80) --end;
  value.remove(end);
  return value;
}

String findTextRecursive(JsonVariantConst value, const char *const *keys, size_t keyCount) {
  if (value.is<JsonObjectConst>()) {
    JsonObjectConst object = value.as<JsonObjectConst>();
    // Match fields at the current level first, mirroring the Windows test tool.
    for (JsonPairConst pair : object) {
      const char *name = pair.key().c_str();
      for (size_t i = 0; i < keyCount; ++i) {
        if (strcmp(name, keys[i]) == 0 && pair.value().is<const char *>()) {
          String text = pair.value().as<String>();
          text.trim();
          if (!text.isEmpty()) return utf8Clip(text, MAX_CONVERSATION_TEXT_BYTES);
        }
      }
    }
    // The server may nest ASR/LLM output under job/result/data objects.
    for (JsonPairConst pair : object) {
      const String found = findTextRecursive(pair.value(), keys, keyCount);
      if (!found.isEmpty()) return found;
    }
  } else if (value.is<JsonArrayConst>()) {
    for (JsonVariantConst item : value.as<JsonArrayConst>()) {
      const String found = findTextRecursive(item, keys, keyCount);
      if (!found.isEmpty()) return found;
    }
  }
  return "";
}

String firstTextField(JsonVariantConst value) {
  // Keep this set aligned with windows_local_asr_remote_dialogue_v4_prefetch.py.
  static const char *keys[] = {
      "asr_text", "transcript", "recognized_text", "recognised_text",
      "input_text", "question_text", "user_text"};
  return findTextRecursive(value, keys, sizeof(keys) / sizeof(keys[0]));
}

String assistantTextField(JsonVariantConst value) {
  // Deliberately do not match a generic nested `text`: segment text is handled
  // separately, while the final job response should use an explicit answer key.
  static const char *keys[] = {
      "answer_text", "assistant_text", "llm_text", "reply", "answer", "response_text"};
  return findTextRecursive(value, keys, sizeof(keys) / sizeof(keys[0]));
}

void setCurrentAssistantText(const String &text) {
  currentAssistantText = utf8Clip(text, MAX_CONVERSATION_TEXT_BYTES);
}

void appendHistoryAssistantText(const String &text) {
  if (text.isEmpty()) return;
  if (!historyAssistantText.isEmpty()) historyAssistantText += " ";
  historyAssistantText += text;
  historyAssistantText = utf8Clip(historyAssistantText, MAX_CONVERSATION_TEXT_BYTES);
}

void saveConversationTurn() {
  if (currentUserText.isEmpty() && historyAssistantText.isEmpty()) return;
  fs::FS &storage = VoiceInput::recordingStorage();
  File probe = storage.open(CONVERSATION_LOG_PATH, FILE_READ);
  const size_t existing = probe ? probe.size() : 0;
  if (probe) probe.close();

  JsonDocument doc;
  doc["at_ms"] = millis();
  doc["user"] = currentUserText;
  doc["assistant"] = historyAssistantText;
  String line;
  serializeJson(doc, line);
  line += '\n';
  if (existing + line.length() > MAX_CONVERSATION_LOG_BYTES) storage.remove(CONVERSATION_LOG_PATH);

  File file = storage.open(CONVERSATION_LOG_PATH, FILE_APPEND);
  if (!file) {
    traceRuntime("conversation_log_error", "无法写入网页对话记录", true);
    return;
  }
  file.print(line);
  file.close();
}

String baseUrlWithScheme(const String &configured, bool https) {
  String value = normalizedUrl(configured);
  if (value.startsWith("https://")) value.remove(0, 8);
  else if (value.startsWith("http://")) value.remove(0, 7);
  while (value.startsWith("/")) value.remove(0, 1);
  return String(https ? "https://" : "http://") + value;
}

String requestBaseUrl() {
  if (!activeServerUrl.isEmpty()) return activeServerUrl;
  return baseUrlWithScheme(settings.serverUrl, true);
}

String dialogueBaseUrl() {
  if (DIALOGUE_TRANSPORT_HTTP) return baseUrlWithScheme(settings.serverUrl, false);
  return requestBaseUrl();
}

String baseFromRequestUrl(const String &url) {
  const int schemeEnd = url.indexOf("://");
  const int hostStart = schemeEnd >= 0 ? schemeEnd + 3 : 0;
  const int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) return normalizedUrl(url);
  return normalizedUrl(url.substring(0, pathStart));
}

void closeCloudConnection() {
  cloudHttp.setReuse(false);
  cloudHttp.end();
  cloudSecureClient.stop();
  cloudPlainClient.stop();
  cloudHttp.setReuse(true);
  cloudConnectionBaseUrl = "";
  lastCloudActivityMs = 0;
}

bool beginCloudRequest(const String &url) {
  const String baseUrl = baseFromRequestUrl(url);
  if (!cloudConnectionBaseUrl.isEmpty() && cloudConnectionBaseUrl != baseUrl) {
    closeCloudConnection();
  }

  cloudHttp.setReuse(true);
  if (url.startsWith("https://")) {
    if (!clockReady) return false;
    cloudSecureClient.setCACert(DIGICERT_GLOBAL_ROOT_G2);
    cloudSecureClient.setHandshakeTimeout(8);
    cloudConnectionBaseUrl = baseUrl;
    return cloudHttp.begin(cloudSecureClient, url);
  }
  cloudConnectionBaseUrl = baseUrl;
  return cloudHttp.begin(cloudPlainClient, url);
}

void endCloudRequest(bool keepAlive = true) {
  cloudHttp.end();
  lastCloudActivityMs = millis();
  if (!keepAlive) closeCloudConnection();
}

bool migrateLegacyLanSettings() {
  if (!settings.wifiSsid.isEmpty()) return false;
  preferences.begin("ai-config", true);
  const String legacySsid = preferences.getString("wifi_ssid", "");
  const String legacyPassword = preferences.getString("wifi_pass", "");
  const uint8_t legacyVolume = preferences.getUChar("volume", 50);
  const bool legacyVolumePot = preferences.getBool("volume_pot", false);
  preferences.end();
  if (legacySsid.isEmpty()) return false;

  settings.wifiSsid = legacySsid;
  settings.wifiPassword = legacyPassword;
  settings.volumePercent = legacyVolume;
  settings.volumePotentiometer = legacyVolumePot;
  preferences.begin("cloud-ai", false);
  bool ok = true;
  ok &= preferences.putString("wifi_ssid", settings.wifiSsid) > 0;
  ok &= preferences.putString("wifi_pass", settings.wifiPassword) > 0 || settings.wifiPassword.isEmpty();
  ok &= preferences.putUChar("volume", settings.volumePercent) > 0;
  ok &= preferences.putBool("volume_pot", settings.volumePotentiometer) > 0;
  preferences.end();
  Serial.println(ok ? F("[配置] 已从局域网固件迁移 Wi-Fi/音量；公网设备 Token 仍需单独配置。")
                    : F("[配置] 找到局域网 Wi-Fi，但写入 cloud-ai NVS 失败。"));
  return ok;
}

void loadSettings() {
  preferences.begin("cloud-ai", true);
  settings.wifiSsid = preferences.getString("wifi_ssid", "");
  settings.wifiPassword = preferences.getString("wifi_pass", "");
  settings.serverUrl = preferences.getString("server_url", DEFAULT_SERVER_URL);
  settings.deviceId = preferences.getString("device_id", hardwareDeviceId());
  settings.deviceToken = preferences.getString("device_token", "");
  settings.volumePercent = preferences.getUChar("volume", 50);
  settings.chineseSpeakerId = preferences.getUChar("spk_zh", 0);
  settings.englishSpeakerId = preferences.getUShort("spk_en", 0);
  settings.speechSpeed = preferences.getFloat("tts_speed", 1.0f);
  settings.volumePotentiometer = preferences.getBool("volume_pot", false);
  preferences.end();
  if (settings.deviceToken.isEmpty() && strlen(CLOUD_PROVISIONED_DEVICE_TOKEN) > 0) {
    settings.deviceId = CLOUD_PROVISIONED_DEVICE_ID;
    settings.deviceToken = CLOUD_PROVISIONED_DEVICE_TOKEN;
    Serial.println(F("[配置] 已载入本机 secrets.h 的设备凭证；Token 不会输出到串口或网页。"));
  }
  migrateLegacyLanSettings();
  settings.serverUrl = normalizedUrl(settings.serverUrl);
  settings.volumePercent = constrain(settings.volumePercent, 0, 100);
  settings.chineseSpeakerId = constrain(settings.chineseSpeakerId, 0, 4);
  settings.englishSpeakerId = constrain(settings.englishSpeakerId, 0, 903);
  settings.speechSpeed = constrain(settings.speechSpeed, 0.5f, 2.0f);
  VoiceInput::setPlaybackVolume(settings.volumePercent);
  VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
}

bool saveSettings() {
  preferences.begin("cloud-ai", false);
  bool ok = true;
  ok &= preferences.putString("wifi_ssid", settings.wifiSsid) > 0 || settings.wifiSsid.isEmpty();
  ok &= preferences.putString("wifi_pass", settings.wifiPassword) > 0 || settings.wifiPassword.isEmpty();
  ok &= preferences.putString("server_url", settings.serverUrl) > 0;
  ok &= preferences.putString("device_id", settings.deviceId) > 0;
  ok &= preferences.putString("device_token", settings.deviceToken) > 0 || settings.deviceToken.isEmpty();
  ok &= preferences.putUChar("volume", settings.volumePercent) > 0;
  ok &= preferences.putUChar("spk_zh", settings.chineseSpeakerId) > 0 || settings.chineseSpeakerId == 0;
  ok &= preferences.putUShort("spk_en", settings.englishSpeakerId) > 0 || settings.englishSpeakerId == 0;
  ok &= preferences.putFloat("tts_speed", settings.speechSpeed) > 0;
  ok &= preferences.putBool("volume_pot", settings.volumePotentiometer) > 0;
  preferences.end();
  return ok;
}

bool connectWifi(bool force = false) {
  if (settings.wifiSsid.isEmpty()) {
    Serial.println(F("[Wi-Fi] 未配置 SSID；请通过加密 BLE 写入 Wi-Fi、设备 ID 和 Token。"));
    traceRuntime("wifi_error", "SSID 未配置", true);
    return false;
  }
  if (WiFi.status() == WL_CONNECTED && !force) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (force) {
    closeCloudConnection();
    WiFi.disconnect(false, false);
    activeServerUrl = "";
    clockReady = false;
    delay(120);
  }
  Serial.printf("[Wi-Fi] 正在连接 %s", settings.wifiSsid.c_str());
  traceRuntime("wifi_connecting", "正在连接 " + settings.wifiSsid);
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(350);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[Wi-Fi] 连接失败。"));
    traceRuntime("wifi_error", "连接失败", true);
    return false;
  }
  Serial.printf("[Wi-Fi] 已连接，IP=%s，RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  traceRuntime("wifi_connected", "IP=" + WiFi.localIP().toString() + ", RSSI=" + String(WiFi.RSSI()));

  configTime(0, 0, "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
  const uint32_t ntpStarted = millis();
  while (time(nullptr) < 1700000000 && millis() - ntpStarted < NTP_TIMEOUT_MS) delay(200);
  clockReady = time(nullptr) >= 1700000000;
  Serial.println(clockReady ? F("[TLS] 时间同步完成，启用 CA 证书校验。")
                            : F("[TLS] 时间同步失败；HTTPS 暂不可用，将按配置尝试 HTTP 回退。"));
  traceRuntime(clockReady ? "time_ready" : "time_error",
               clockReady ? "NTP 完成，CA 校验可用" : "NTP 失败，将尝试 HTTP 回退",
               !clockReady);
  return true;
}

void addAuthHeaders(HTTPClient &http) {
  http.addHeader("X-Device-ID", settings.deviceId);
  http.addHeader("Authorization", "Bearer " + settings.deviceToken);
}

String statusJson(const String &message = "") {
  JsonDocument doc;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["server_url"] = settings.serverUrl;
  doc["active_server_url"] = activeServerUrl;
  doc["dialogue_base_url"] = dialogueBaseUrl();
  doc["ssid"] = settings.wifiSsid;
  doc["phone_url"] = WiFi.status() == WL_CONNECTED ? "http://" + WiFi.localIP().toString() + "/" : "";
  doc["tls_ca_verified"] = activeServerUrl.startsWith("https://");
  doc["clock_ready"] = clockReady;
  doc["device_id"] = settings.deviceId;
  doc["token_configured"] = !settings.deviceToken.isEmpty();
  doc["volume_percent"] = settings.volumePercent;
  doc["chinese_speaker_id"] = settings.chineseSpeakerId;
  doc["english_speaker_id"] = settings.englishSpeakerId;
  doc["speech_speed"] = settings.speechSpeed;
  doc["volume_mode"] = settings.volumePotentiometer ? "pot" : "fixed";
  doc["audio_ready"] = VoiceInput::ready();
  doc["busy"] = busy;
  doc["dialogue_stage"] = dialogueStage;
  doc["last_error"] = lastDialogueError;
  doc["last_http_code"] = lastHttpCode;
  doc["current_job_id"] = currentJobId;
  doc["current_segment_index"] = currentSegmentIndex;
  doc["stage_elapsed_ms"] = millis() - stageStartedMs;
  if (!message.isEmpty()) doc["message"] = message;
  String output;
  serializeJson(doc, output);
  return output;
}

String runtimeJson() {
  JsonDocument doc;
  doc["stage"] = dialogueStage;
  doc["stage_elapsed_ms"] = millis() - stageStartedMs;
  doc["dialogue_elapsed_ms"] = busy ? millis() - dialogueStartedMs : 0;
  doc["last_error"] = lastDialogueError;
  doc["last_http_code"] = lastHttpCode;
  doc["job_id"] = currentJobId;
  doc["segment_index"] = currentSegmentIndex;
  doc["busy"] = busy;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  doc["active_server_url"] = activeServerUrl;
  doc["dialogue_base_url"] = dialogueBaseUrl();
  doc["tls_ca_verified"] = activeServerUrl.startsWith("https://");
  doc["free_heap"] = ESP.getFreeHeap();
  doc["minimum_free_heap"] = ESP.getMinFreeHeap();
  doc["largest_free_block"] = ESP.getMaxAllocHeap();
  doc["cloud_base_url"] = cloudConnectionBaseUrl;
  doc["cloud_keepalive"] = cloudHttp.connected();
  doc["cloud_idle_ms"] = lastCloudActivityMs > 0 ? millis() - lastCloudActivityMs : 0;
  doc["current_user_text"] = currentUserText;
  doc["current_assistant_text"] = currentAssistantText;
  JsonArray events = doc["events"].to<JsonArray>();
  const size_t first = (runtimeEventHead + RUNTIME_EVENT_COUNT - runtimeEventSize) % RUNTIME_EVENT_COUNT;
  for (size_t i = 0; i < runtimeEventSize; ++i) {
    const RuntimeEvent &event = runtimeEvents[(first + i) % RUNTIME_EVENT_COUNT];
    JsonObject item = events.add<JsonObject>();
    item["at_ms"] = event.atMs;
    item["text"] = event.text;
  }
  String output;
  serializeJson(doc, output);
  return output;
}

void publishStatus(const String &message = "") {
  if (!statusCharacteristic) return;
  statusCharacteristic->setValue(statusJson(message));
  if (bleConnected) statusCharacteristic->notify();
}

bool serverHealthAt(const String &baseUrl) {
  if (WiFi.status() != WL_CONNECTED) return false;
  cloudHttp.setConnectTimeout(4000);
  cloudHttp.setTimeout(6000);
  if (!beginCloudRequest(baseUrl + "/voice-api/v1/health")) return false;
  const int code = cloudHttp.GET();
  lastHttpCode = code;
  endCloudRequest(baseUrl.startsWith("http://"));
  return code == HTTP_CODE_OK;
}

bool selectPreferredServer() {
  if (WiFi.status() != WL_CONNECTED) return false;

  const String httpsBase = baseUrlWithScheme(settings.serverUrl, true);
  if (serverHealthAt(httpsBase)) {
    activeServerUrl = httpsBase;
    Serial.printf("[NET] HTTPS selected: %s\n", activeServerUrl.c_str());
    traceRuntime("server_ready", "HTTPS + CA 校验通过");
    return true;
  }

  closeCloudConnection();
  const String httpBase = baseUrlWithScheme(settings.serverUrl, false);
  if (serverHealthAt(httpBase)) {
    activeServerUrl = httpBase;
    Serial.printf("[NET] HTTPS unavailable, fallback HTTP: %s\n", activeServerUrl.c_str());
    traceRuntime("server_fallback", "HTTPS 不可用，已回退 HTTP");
    return true;
  }

  activeServerUrl = "";
  closeCloudConnection();
  Serial.println("[NET] HTTPS and HTTP are both unavailable");
  traceRuntime("server_error", "HTTPS 和 HTTP 均不可用", true);
  return false;
}

bool syncVoiceSettings() {
  if (WiFi.status() != WL_CONNECTED || settings.deviceToken.isEmpty()) return false;
  if (activeServerUrl.isEmpty() && !selectPreferredServer()) return false;
  cloudHttp.setConnectTimeout(5000);
  cloudHttp.setTimeout(10000);
  if (!beginCloudRequest(requestBaseUrl() + "/voice-api/v1/device-settings")) return false;
  addAuthHeaders(cloudHttp);
  cloudHttp.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["chinese_speaker_id"] = settings.chineseSpeakerId;
  doc["english_speaker_id"] = settings.englishSpeakerId;
  doc["speed"] = settings.speechSpeed;
  String payload;
  serializeJson(doc, payload);
  const int code = cloudHttp.PUT(payload);
  lastHttpCode = code;
  endCloudRequest(requestBaseUrl().startsWith("http://"));
  traceRuntime(code == HTTP_CODE_OK ? "voice_settings_synced" : "voice_settings_error",
               "HTTP " + String(code), code != HTTP_CODE_OK);
  return code == HTTP_CODE_OK;
}

bool applyConfig(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) return false;
  if (doc["ssid"].is<const char *>()) settings.wifiSsid = doc["ssid"].as<String>();
  if (doc["wifi_password"].is<const char *>()) settings.wifiPassword = doc["wifi_password"].as<String>();
  if (doc["server_url"].is<const char *>()) settings.serverUrl = normalizedUrl(doc["server_url"].as<String>());
  if (doc["device_id"].is<const char *>()) settings.deviceId = doc["device_id"].as<String>();
  if (doc["device_token"].is<const char *>()) settings.deviceToken = doc["device_token"].as<String>();
  if (doc["volume_percent"].is<int>()) {
    settings.volumePercent = constrain(doc["volume_percent"].as<int>(), 0, 100);
    settings.volumePotentiometer = false;
  }
  if (doc["volume_mode"].is<const char *>()) {
    String mode = doc["volume_mode"].as<String>();
    mode.toLowerCase();
    if (mode != "fixed" && mode != "pot") return false;
    settings.volumePotentiometer = mode == "pot";
  }
  const bool voiceChanged = doc["chinese_speaker_id"].is<int>() ||
                            doc["english_speaker_id"].is<int>() ||
                            doc["speech_speed"].is<float>() || doc["speech_speed"].is<int>();
  if (doc["chinese_speaker_id"].is<int>()) settings.chineseSpeakerId = constrain(doc["chinese_speaker_id"].as<int>(), 0, 4);
  if (doc["english_speaker_id"].is<int>()) settings.englishSpeakerId = constrain(doc["english_speaker_id"].as<int>(), 0, 903);
  if (doc["speech_speed"].is<float>() || doc["speech_speed"].is<int>()) settings.speechSpeed = constrain(doc["speech_speed"].as<float>(), 0.5f, 2.0f);
  if (settings.wifiSsid.length() > 32 || settings.wifiPassword.length() > 63 ||
      settings.serverUrl.length() > 180 ||
      !(settings.serverUrl.startsWith("http://") || settings.serverUrl.startsWith("https://")) ||
      settings.deviceId.length() < 3 || settings.deviceId.length() > 64 || settings.deviceToken.length() > 256) return false;
  VoiceInput::setPlaybackVolume(settings.volumePercent);
  VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
  activeServerUrl = "";
  closeCloudConnection();
  if (!saveSettings()) return false;
  traceRuntime("config_saved", "设备配置已写入 NVS");
  pendingWifiReconnect = doc["connect"] | true;
  pendingVoiceSync = pendingVoiceSync || voiceChanged;
  return true;
}

void queueBleRequest(BleRequestType type, const String &value) {
  if (!bleQueue || value.length() > MAX_BLE_PAYLOAD) {
    publishStatus("payload_too_large");
    return;
  }
  BleRequest request{};
  request.type = type;
  memcpy(request.payload, value.c_str(), value.length());
  request.payload[value.length()] = '\0';
  if (xQueueSend(bleQueue, &request, 0) != pdTRUE) publishStatus("busy");
}

class ConfigCallbacks final : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *characteristic) override { characteristic->setValue(statusJson()); }
  void onWrite(BLECharacteristic *characteristic) override {
    queueBleRequest(BleRequestType::Config, characteristic->getValue());
  }
};

class CommandCallbacks final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    queueBleRequest(BleRequestType::Command, characteristic->getValue());
  }
};

class ServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { bleConnected = true; }
  void onDisconnect(BLEServer *) override { bleConnected = false; BLEDevice::startAdvertising(); }
};

void startBle() {
  bleQueue = xQueueCreate(4, sizeof(BleRequest));
  char name[24];
  snprintf(name, sizeof(name), "ESP32-CLOUD-%04X", static_cast<uint16_t>(ESP.getEfuseMac()));
  bleDeviceName = name;
  BLEDevice::init(name);
  BLEDevice::setMTU(517);
  BLESecurity *security = new BLESecurity();
  blePin = 100000 + static_cast<uint32_t>(ESP.getEfuseMac() % 900000);
  security->setPassKey(true, blePin);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setAuthenticationMode(true, true, true);
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService *service = server->createService(BLE_SERVICE_UUID);
  BLECharacteristic *config = service->createCharacteristic(BLE_CONFIG_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ_AUTHEN | BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  config->setCallbacks(new ConfigCallbacks());
  BLECharacteristic *command = service->createCharacteristic(BLE_COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_AUTHEN);
  command->setCallbacks(new CommandCallbacks());
  statusCharacteristic = service->createCharacteristic(BLE_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY | BLECharacteristic::PROPERTY_READ_AUTHEN);
  statusCharacteristic->addDescriptor(new BLE2902());
  statusCharacteristic->setValue(statusJson("ready"));
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(BLE_SERVICE_UUID);
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] %s 持续可用，配对 PIN=%06lu。\n", name,
                static_cast<unsigned long>(blePin));
}

void processBleRequests() {
  if (!bleQueue) return;
  BleRequest request{};
  while (xQueueReceive(bleQueue, &request, 0) == pdTRUE) {
    String value(request.payload);
    if (request.type == BleRequestType::Config) {
      const bool ok = applyConfig(value);
      publishStatus(ok ? "config_saved" : "invalid_config");
      continue;
    }
    value.trim();
    value.toLowerCase();
    if (value == "connect") pendingWifiReconnect = true;
    else if (value == "sync_voice") pendingVoiceSync = true;
    else if (value == "status") publishStatus("status");
    else if (value == "reboot") {
      publishStatus("rebooting");
      delay(200);
      ESP.restart();
    } else publishStatus("unknown_command");
  }
}

void startPhoneWeb() {
  if (webStarted || WiFi.status() != WL_CONNECTED) return;
  phoneWeb.on("/", HTTP_GET, []() {
    String page;
    page.reserve(9000);
    page = F("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>ESP32 云语音设置</title><style>body{font-family:system-ui;max-width:720px;"
             "margin:24px auto;padding:0 16px;background:#f4f7fb;color:#172033}fieldset{background:#fff;"
             "border:1px solid #d9e0ea;border-radius:10px;padding:16px;margin:14px 0}label{display:block;"
             "margin:10px 0 4px}input,select{box-sizing:border-box;width:100%;padding:10px;border:1px solid #aeb8c8;"
             "border-radius:6px}.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}button{padding:11px 18px;"
             "border:0;border-radius:6px;background:#1267d6;color:#fff}.ok{color:#08783f}.warn{color:#a04b00}"
             "code{overflow-wrap:anywhere}.chat{display:flex;flex-direction:column;gap:10px;max-height:420px;overflow:auto;padding:4px}.msg{padding:10px 12px;border-radius:10px;white-space:pre-wrap;overflow-wrap:anywhere}.user{background:#e7f0ff;margin-left:12%}.assistant{background:#eef7ee;margin-right:12%}.meta{font-size:12px;color:#667085;margin-bottom:4px}@media(max-width:600px){.row{grid-template-columns:1fr}.user,.assistant{margin:0}}</style></head><body>"
             "<h1>ESP32 云语音设置</h1>");
    page += "<p class='ok'>Wi-Fi 已连接：" + htmlEscape(settings.wifiSsid) +
            "，IP：<code>" + WiFi.localIP().toString() + "</code></p>";
    page += "<p>当前云线路：<code>" +
            htmlEscape(activeServerUrl.isEmpty() ? String("尚未检测") : activeServerUrl) + "</code></p>";
    page += settings.deviceToken.isEmpty()
                ? F("<p class='warn'>尚未配置设备 Token，当前不能发起对话。</p>")
                : F("<p class='ok'>设备 Token 已配置（不会在网页回显）。</p>");
    page += F("<fieldset><legend>实时对话流程</legend><p id='flow-summary'>正在读取运行状态…</p>"
              "<p id='flow-cloud'></p>"
              "<p id='flow-error' class='warn'></p><pre id='flow-events' style='white-space:pre-wrap;"
              "background:#101828;color:#d0f0df;padding:12px;border-radius:6px;max-height:320px;overflow:auto'>"
              "</pre><p><small>页面每秒刷新；记录联网、证书、鉴权、上传、分段等待和播放错误。"
              "Token、Wi-Fi 密码不会进入日志。</small></p></fieldset>");
    page += F("<fieldset><legend>当前对话</legend><div id='current-chat' class='chat'>等待新一轮对话…</div>"
              "<p><small>这里只显示当前正在处理/播放的话，不累计整段回答。</small></p></fieldset>"
              "<fieldset><legend>历史记录</legend><div id='history-chat' class='chat'>正在读取历史记录…</div>"
              "<p><small>每轮完成后只保存一次完整的‘你 / AI’记录；达到约 24 KB 后自动从新记录开始。</small></p></fieldset>");
    page += F("<form method='post' action='/save'><fieldset><legend>联网和设备凭证</legend>"
              "<label>Wi-Fi SSID</label><input name='ssid' maxlength='32' value='");
    page += htmlEscape(settings.wifiSsid);
    page += F("'><label>Wi-Fi 密码（留空保持原值）</label><input type='password' name='wifi_password' maxlength='63'>"
              "<label>云服务器地址</label><input name='server_url' maxlength='180' value='");
    page += htmlEscape(settings.serverUrl);
    page += F("'><label>设备 ID</label><input name='device_id' maxlength='64' value='");
    page += htmlEscape(settings.deviceId);
    page += F("'><label>设备 Token（留空保持原值）</label><input type='password' name='device_token' maxlength='256'>"
              "</fieldset><fieldset><legend>本设备个性化语音</legend><div class='row'><div>"
              "<label>中文音色 0～4</label><input type='number' name='chinese_speaker_id' min='0' max='4' value='");
    page += String(settings.chineseSpeakerId);
    page += F("'></div><div><label>英文音色 0～903</label><input type='number' name='english_speaker_id' min='0' max='903' value='");
    page += String(settings.englishSpeakerId);
    page += F("'></div></div><div class='row'><div><label>语速 0.5～2.0</label>"
              "<input type='number' name='speech_speed' min='0.5' max='2' step='0.05' value='");
    page += String(settings.speechSpeed, 2);
    page += F("'></div><div><label>音量</label><input type='number' name='volume_percent' min='0' max='100' value='");
    page += String(settings.volumePercent);
    page += F("'></div></div><label>音量模式</label><select name='volume_mode'><option value='fixed'");
    if (!settings.volumePotentiometer) page += F(" selected");
    page += F(">固定音量</option><option value='pot'");
    if (settings.volumePotentiometer) page += F(" selected");
    page += F(">GPIO1 电位器</option></select></fieldset><fieldset><legend>保存确认</legend>"
              "<p>输入串口显示的 6 位 BLE 配对 PIN，防止同一局域网内其他人修改设置。</p>"
              "<label>设备 PIN</label><input type='password' inputmode='numeric' name='pin' minlength='6' maxlength='6' required>"
              "<p><button type='submit'>保存、重连并同步服务器</button></p></fieldset></form>"
              "<p><a href='/status'>查看完整 JSON 状态</a></p>"
              "<script>const labels={booting:'启动',idle:'待机',checking_network:'检查网络',"
              "wifi_connecting:'连接Wi-Fi',wifi_connected:'Wi-Fi已连接',time_ready:'时间已同步',"
              "time_error:'时间同步失败',server_ready:'HTTPS就绪',server_fallback:'HTTP回退',"
              "server_error:'服务器不可用',recording:'录音',uploading:'上传录音',upload_retry:'上传重试',"
              "audio_suspended:'音频已释放',audio_suspend_error:'音频释放失败',"
              "waiting_segment:'等待语音分段',playing:'播放',conversation_text:'同步对话文字',completed:'完成',error:'失败'};"
              "function esc(s){return String(s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}"
              "function bubble(role,text,label){if(!text)return '';return '<div class=\"msg '+role+'\"><div class=\"meta\">'+label+'</div>'+esc(text)+'</div>';}"
              "async function refreshConversation(d){try{let c='';c+=bubble('user',d.current_user_text,'你 · 当前');c+=bubble('assistant',d.current_assistant_text,'AI · 当前');document.getElementById('current-chat').innerHTML=c||'<span class=\"meta\">等待新一轮对话…</span>';const r=await fetch('/conversation.log',{cache:'no-store'});let h='';if(r.ok){const t=await r.text();for(const line of t.trim().split('\\n')){if(!line)continue;try{const x=JSON.parse(line);h+=bubble('user',x.user,'你');h+=bubble('assistant',x.assistant,'AI');}catch(_){}}}document.getElementById('history-chat').innerHTML=h||'<span class=\"meta\">暂无历史记录。</span>';}catch(e){document.getElementById('history-chat').textContent='对话记录读取失败：'+e;}}"
              "async function refreshFlow(){try{const r=await fetch('/runtime',{cache:'no-store'});"
              "const d=await r.json();const s=labels[d.stage]||d.stage;"
              "document.getElementById('flow-summary').textContent='阶段：'+s+' ｜ HTTP：'+"
              "(d.last_http_code||'-')+' ｜ 分段：'+d.segment_index+' ｜ Wi-Fi：'+d.wifi+"
              "' '+d.rssi+' dBm ｜ 堆：'+d.free_heap+' ｜ 最大连续块：'+d.largest_free_block+"
              "' ｜ 历史最低：'+d.minimum_free_heap;"
              "document.getElementById('flow-cloud').textContent='云连接：'+"
              "(d.cloud_keepalive?'保持中':'未保持')+' ｜ 空闲：'+(d.cloud_idle_ms||0)+"
              "' ms ｜ 探测：'+(d.active_server_url||'-')+' ｜ 对话：'+(d.dialogue_base_url||'-');"
              "document.getElementById('flow-error').textContent=d.last_error?'最近错误：'+d.last_error:'';"
              "document.getElementById('flow-events').textContent=d.events.map(e=>'['+(e.at_ms/1000).toFixed(1)+'s] '+e.text).join('\\n');"
              "await refreshConversation(d);"
              "}catch(e){document.getElementById('flow-error').textContent='状态读取失败：'+e;}"
              "setTimeout(refreshFlow,1000)}refreshFlow();</script></body></html>");
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8", page);
  });

  phoneWeb.on("/save", HTTP_POST, []() {
    if (phoneWeb.arg("pin").toInt() != static_cast<int>(blePin)) {
      phoneWeb.send(403, "text/plain; charset=utf-8", "设备 PIN 错误，配置未修改。");
      return;
    }
    JsonDocument doc;
    doc["ssid"] = phoneWeb.arg("ssid");
    if (!phoneWeb.arg("wifi_password").isEmpty()) doc["wifi_password"] = phoneWeb.arg("wifi_password");
    doc["server_url"] = phoneWeb.arg("server_url");
    doc["device_id"] = phoneWeb.arg("device_id");
    if (!phoneWeb.arg("device_token").isEmpty()) doc["device_token"] = phoneWeb.arg("device_token");
    doc["chinese_speaker_id"] = phoneWeb.arg("chinese_speaker_id").toInt();
    doc["english_speaker_id"] = phoneWeb.arg("english_speaker_id").toInt();
    doc["speech_speed"] = phoneWeb.arg("speech_speed").toFloat();
    doc["volume_percent"] = phoneWeb.arg("volume_percent").toInt();
    doc["volume_mode"] = phoneWeb.arg("volume_mode");
    doc["connect"] = true;
    String payload;
    serializeJson(doc, payload);
    if (!applyConfig(payload)) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "配置格式无效，未保存。");
      return;
    }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8",
                  "<!doctype html><meta charset='utf-8'><meta name='viewport' content='width=device-width'>"
                  "<h2>配置已保存</h2><p>设备正在重新联网并同步云端，IP 可能变化。约 15 秒后重新打开设备地址。</p>");
  });

  phoneWeb.on("/status", HTTP_GET, []() {
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", statusJson());
  });
  phoneWeb.on("/runtime", HTTP_GET, []() {
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", runtimeJson());
  });
  phoneWeb.on("/conversation.log", HTTP_GET, []() {
    File file = VoiceInput::recordingStorage().open(CONVERSATION_LOG_PATH, FILE_READ);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    if (!file) {
      phoneWeb.send(200, "application/x-ndjson; charset=utf-8", "");
      return;
    }
    phoneWeb.streamFile(file, "application/x-ndjson; charset=utf-8");
    file.close();
  });
  phoneWeb.on("/health", HTTP_GET, []() {
    phoneWeb.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  });
  phoneWeb.on("/favicon.ico", HTTP_GET, []() { phoneWeb.send(204); });
  phoneWeb.onNotFound([]() { phoneWeb.send(404, "text/plain; charset=utf-8", "Not found"); });
  phoneWeb.begin();
  webStarted = true;
  Serial.println("[设备网页] http://" + WiFi.localIP().toString() + "/");
}

void serviceLocalWebFor(uint32_t durationMs) {
  const uint32_t started = millis();
  do {
    if (webStarted) phoneWeb.handleClient();
    delay(5);
  } while (millis() - started < durationMs);
}

bool submitRecording(String &jobId) {
  File probe = VoiceInput::recordingStorage().open(RECORDING_PATH, FILE_READ);
  if (!probe) {
    traceRuntime("error", "无法打开录音文件", true);
    return false;
  }
  const size_t recordingBytes = probe.size();
  probe.close();
  if (activeServerUrl.isEmpty() && !selectPreferredServer()) return false;

  traceRuntime("uploading", "正在上传 " + String(recordingBytes) + " bytes");
  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    File recording = VoiceInput::recordingStorage().open(RECORDING_PATH, FILE_READ);
    if (!recording) {
      traceRuntime("error", "无法重新打开录音文件", true);
      return false;
    }

    int code = HTTPC_ERROR_CONNECTION_REFUSED;
    String response;
    cloudHttp.setConnectTimeout(7000);
    cloudHttp.setTimeout(30000);
    if (beginCloudRequest(dialogueBaseUrl() + "/voice-api/v1/dialogue")) {
      addAuthHeaders(cloudHttp);
      cloudHttp.addHeader("Content-Type", "audio/L16");
      code = cloudHttp.sendRequest("POST", &recording, recordingBytes);
      if (code == HTTP_CODE_ACCEPTED) response = cloudHttp.getString();
      endCloudRequest();
    }
    recording.close();
    lastHttpCode = code;

    if (code == HTTP_CODE_ACCEPTED) {
      JsonDocument doc;
      const bool ok = !deserializeJson(doc, response) && doc["job_id"].is<const char *>();
      if (!ok) {
        traceRuntime("error", "服务器未返回有效 job_id", true);
        return false;
      }
      jobId = doc["job_id"].as<String>();
      currentJobId = jobId;
      const String submittedUserText = firstTextField(doc.as<JsonVariantConst>());
      if (!submittedUserText.isEmpty()) currentUserText = submittedUserText;
      traceRuntime("waiting_segment", "任务已创建，等待第 0 段");
      return true;
    }

    const String reason = code < 0 ? HTTPClient::errorToString(code) : "HTTP " + String(code);
    Serial.printf("[HTTP] submit=%d (%s), attempt=%u, heap=%u, largest=%u\n",
                  code, reason.c_str(), attempt,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));

    // Retry only failures that occur before an HTTP request is established.
    // Retrying send/read failures could create a duplicate server job.
    if (attempt == 1 &&
        (code == HTTPC_ERROR_CONNECTION_REFUSED || code == HTTPC_ERROR_NOT_CONNECTED)) {
      traceRuntime("upload_retry", reason + "；重置云端连接后重试一次");
      closeCloudConnection();
      delay(600);
      continue;
    }
    traceRuntime("error", "提交对话失败：" + reason + " (" + String(code) + ")", true);
    return false;
  }
  return false;
}

bool refreshJobConversationText(const String &jobId, bool finalRead = false) {
  JsonDocument doc;
  cloudHttp.setConnectTimeout(5000);
  cloudHttp.setTimeout(10000);
  const String url = dialogueBaseUrl() + "/voice-api/v1/jobs/" + jobId;
  if (!beginCloudRequest(url)) return false;
  addAuthHeaders(cloudHttp);
  const int code = cloudHttp.GET();
  lastHttpCode = code;
  if (code != HTTP_CODE_OK) {
    endCloudRequest();
    return false;
  }
  const DeserializationError error = deserializeJson(doc, cloudHttp.getStream());
  endCloudRequest();
  if (error) return false;

  const String userText = firstTextField(doc.as<JsonVariantConst>());
  if (!userText.isEmpty()) currentUserText = userText;

  const String answerText = assistantTextField(doc.as<JsonVariantConst>());
  if (!answerText.isEmpty()) historyAssistantText = answerText;

  if (finalRead && (!userText.isEmpty() || !answerText.isEmpty())) {
    traceRuntime("conversation_text", "已从任务 JSON 更新对话文字");
  }
  return !userText.isEmpty() || !answerText.isEmpty();
}

bool playSegment(const String &jobId, int index, size_t expectedBytes) {
  cloudHttp.setConnectTimeout(5000);
  cloudHttp.setTimeout(30000);
  const String path = dialogueBaseUrl() + "/voice-api/v1/jobs/" + jobId + "/segments/" + String(index) + "/audio";
  if (!beginCloudRequest(path)) {
    traceRuntime("error", "无法建立分段下载连接", true);
    return false;
  }
  addAuthHeaders(cloudHttp);
  const int code = cloudHttp.GET();
  lastHttpCode = code;
  if (code != HTTP_CODE_OK) {
    endCloudRequest();
    traceRuntime("error", "下载分段 " + String(index) + " 失败，HTTP " + String(code), true);
    return false;
  }
  const int announced = cloudHttp.getSize();
  if ((announced >= 0 && static_cast<size_t>(announced) != expectedBytes) || expectedBytes == 0) {
    endCloudRequest();
    traceRuntime("error", "分段长度不一致", true);
    return false;
  }
  WiFiClient *stream = cloudHttp.getStreamPtr();
  currentSegmentIndex = index;
  traceRuntime("playing", "分段 " + String(index) + "，" + String(expectedBytes) + " bytes");
  const bool ok = VoiceInput::playPcmStream(*stream, expectedBytes, true);
  endCloudRequest();
  if (!ok) traceRuntime("error", "分段 " + String(index) + " 播放失败", true);
  return ok;
}

bool waitAndPlay(const String &jobId) {
  int after = -1;
  uint32_t delayMs = 250;
  uint32_t lastJobTextPollMs = 0;
  const uint32_t started = millis();
  while (millis() - started < JOB_TIMEOUT_MS) {
    if (digitalRead(VoiceInput::PIN_VOICE_BUTTON) == LOW) {
      traceRuntime("error", "等待语音时检测到 BOOT 按下，本轮取消", true);
      return false;
    }
    if (webStarted) phoneWeb.handleClient();
    // /segments is optimized for audio delivery and may not expose ASR text.
    // The Windows reference tool reads /jobs/{id}; do the same at a low rate
    // until the user's recognized sentence becomes available.
    if (currentUserText.isEmpty() &&
        (lastJobTextPollMs == 0 || millis() - lastJobTextPollMs >= 800)) {
      refreshJobConversationText(jobId, false);
      lastJobTextPollMs = millis();
    }
    JsonDocument doc;
    cloudHttp.setConnectTimeout(5000);
    cloudHttp.setTimeout(10000);
    const String url = dialogueBaseUrl() + "/voice-api/v1/jobs/" + jobId +
                       "/segments?after=" + String(after);
    if (!beginCloudRequest(url)) {
      traceRuntime("error", "无法建立任务轮询连接", true);
      return false;
    }
    addAuthHeaders(cloudHttp);
    const int code = cloudHttp.GET();
    lastHttpCode = code;
    if (code != HTTP_CODE_OK) {
      endCloudRequest();
      traceRuntime("waiting_segment", "轮询 HTTP " + String(code) + "，准备重试");
      serviceLocalWebFor(delayMs);
      delayMs = min(delayMs * 2, 2000UL);
      continue;
    }
    const DeserializationError error = deserializeJson(doc, cloudHttp.getStream());
    endCloudRequest();
    if (error) {
      traceRuntime("error", "分段 JSON 解析失败：" + String(error.c_str()), true);
      return false;
    }
    const String polledUserText = firstTextField(doc.as<JsonVariantConst>());
    if (!polledUserText.isEmpty()) currentUserText = polledUserText;
    for (JsonObject segment : doc["segments"].as<JsonArray>()) {
      const int index = segment["index"] | -1;
      const size_t byteCount = segment["byte_count"] | 0;
      setState(DeviceState::Playing);
      if (index != after + 1) {
        traceRuntime("error", "分段序号不连续，expected=" + String(after + 1) +
                               ", actual=" + String(index), true);
        return false;
      }
      String segmentText;
      if (segment["text"].is<const char *>()) segmentText = segment["text"].as<String>();
      else if (segment["assistant_text"].is<const char *>()) segmentText = segment["assistant_text"].as<String>();
      segmentText.trim();
      setCurrentAssistantText(segmentText);
      appendHistoryAssistantText(segmentText);
      if (!playSegment(jobId, index, byteCount)) return false;
      after = index;
      delayMs = 250;
    }
    const String status = doc["status"] | "";
    if (status == "completed") {
      if (after < 0) {
        traceRuntime("error", "任务完成但没有可播放分段", true);
        return false;
      }
      // Prefer the authoritative final job JSON. If it is temporarily
      // unavailable, keep the assistant text assembled from segment `text`.
      refreshJobConversationText(jobId, true);
      traceRuntime("completed", "已播放 " + String(after + 1) + " 个分段");
      saveConversationTurn();
      // Keep the user's recognized question visible after this turn finishes.
      // The current AI segment is no longer speaking, so clear only that field.
      // currentUserText is replaced when the next dialogue starts.
      currentAssistantText = "";
      historyAssistantText = "";
      return true;
    }
    if (status == "failed") {
      const String serverError = doc["error"] | "服务器任务失败";
      traceRuntime("error", serverError, true);
      return false;
    }
    traceRuntime("waiting_segment", "等待分段 " + String(after + 1));
    serviceLocalWebFor(delayMs);
    delayMs = min(delayMs * 2, 2000UL);
  }
  traceRuntime("error", "等待语音分段超时", true);
  return false;
}

void runDialogue() {
  if (busy) return;
  busy = true;
  dialogueStartedMs = millis();
  lastDialogueError = "";
  currentUserText = "";
  currentAssistantText = "";
  historyAssistantText = "";
  lastHttpCode = 0;
  currentJobId = "";
  currentSegmentIndex = -1;
  traceRuntime("checking_network", "开始新一轮对话");
  if (!VoiceInput::ready()) {
    Serial.println(F("[对话] I2S/录音文件系统未就绪。"));
    publishStatus("audio_not_ready");
    traceRuntime("error", "I2S/录音文件系统未就绪", true);
    setState(DeviceState::Error);
    busy = false;
    return;
  }
  // Startup/configuration already selected and verified activeServerUrl. Avoid
  // opening a throw-away TLS health-check connection immediately before the
  // upload; that pattern can fragment the small internal heap on ESP32-S3.
  if (!connectWifi() || settings.deviceToken.isEmpty() ||
      (activeServerUrl.isEmpty() && !selectPreferredServer())) {
    Serial.println(settings.deviceToken.isEmpty()
                       ? F("[对话] 尚未配置公网设备 Token；请打开设备网页或使用 BLE 配置。")
                       : F("[对话] 网络或云服务器不可用。"));
    publishStatus("input_or_config_error");
    traceRuntime("error", settings.deviceToken.isEmpty() ? "设备 Token 未配置" : "网络或云服务器不可用", true);
    setState(DeviceState::Error);
    busy = false;
    return;
  }
  setState(DeviceState::Recording);
  traceRuntime("recording", "按住 BOOT 录音，松开发送");
  publishStatus("recording");
  Serial.println(F("[按键对话] 正在录音；松开 BOOT 后发送，最长 20 秒。"));
  VoiceInput::AudioStats stats;
  const bool recorded = VoiceInput::recordPcmPushToTalk(RECORDING_PATH, RECORD_MINIMUM_MS, RECORD_MAXIMUM_MS, stats);
  if (!recorded || stats.rms < 80 || stats.peak < 250) {
    Serial.printf("[按键对话] 录音无效，RMS=%u，Peak=%u\n", stats.rms, stats.peak);
    publishStatus("input_or_config_error");
    traceRuntime("error", "录音无效，RMS=" + String(stats.rms) + ", Peak=" + String(stats.peak), true);
    setState(DeviceState::Error);
    busy = false;
    return;
  }
  Serial.printf("[按键对话] 录音 %lu ms，%u bytes，RMS=%u，Peak=%u\n",
                static_cast<unsigned long>(stats.durationMs), static_cast<unsigned>(stats.fileBytes),
                stats.rms, stats.peak);
  if (VoiceInput::releaseAudioHardware()) {
    traceRuntime("audio_suspended", "I2S/DMA 已释放，准备上传；heap=" +
                                      String(ESP.getFreeHeap()) + ", largest=" +
                                      String(ESP.getMaxAllocHeap()));
  } else {
    traceRuntime("audio_suspend_error", "I2S/DMA 释放失败，继续尝试上传", true);
  }
  String jobId;
  setState(DeviceState::Processing);
  traceRuntime("uploading", "录音完成，准备上传");
  publishStatus("uploading");
  const bool ok = submitRecording(jobId) && waitAndPlay(jobId);
  publishStatus(ok ? "completed" : "dialogue_failed");
  if (!ok && dialogueStage != "error") traceRuntime("error", "对话流程失败", true);
  setState(ok ? DeviceState::Idle : DeviceState::Error);
  busy = false;
}

void printStatus() {
  Serial.println(F("\n[云端语音设备状态]"));
  Serial.println(statusJson());
  Serial.printf("I2S: BCLK=%d WS=%d MIC_SD=%d AMP_DIN=%d AMP_SD=%d\n",
                VoiceInput::PIN_I2S_BCLK, VoiceInput::PIN_I2S_WS,
                VoiceInput::PIN_MIC_DATA, VoiceInput::PIN_AMP_DATA, VoiceInput::PIN_AMP_SD);
  Serial.printf("BLE: %s, pairing PIN=%06lu\n", bleDeviceName.c_str(),
                static_cast<unsigned long>(blePin));
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Device page: http://" + WiFi.localIP().toString() + "/");
  }
  Serial.printf("Health: %s\n", activeServerUrl.isEmpty() ? "failed" : "ok");
}

void processConsole() {
  while (Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\n' || value == '\r') {
      serialLine.trim();
      if (serialLine == "/status") printStatus();
      else if (serialLine == "/voice") runDialogue();
      else if (serialLine == "/connect") pendingWifiReconnect = true;
      else if (serialLine == "/sync_voice") pendingVoiceSync = true;
      else if (serialLine == "/reboot") ESP.restart();
      serialLine = "";
    } else if (serialLine.length() < 512) serialLine += value;
  }
}

void processPendingNetworkActions() {
  if (busy) return;
  if (pendingWifiReconnect) {
    pendingWifiReconnect = false;
    const bool connected = connectWifi(true);
    if (connected) {
      startPhoneWeb();
      selectPreferredServer();
    }
    publishStatus(connected ? "connected" : "wifi_failed");
  }
  if (pendingVoiceSync) {
    pendingVoiceSync = false;
    const bool ok = connectWifi() && syncVoiceSettings();
    publishStatus(ok ? "voice_synced" : "voice_sync_failed");
  }
}

}  // namespace CloudDevice

void setup() {
  using namespace CloudDevice;
  Serial.begin(SERIAL_BAUD);
  delay(350);
  traceRuntime("booting", "固件启动");
  setState(DeviceState::Booting);
  loadSettings();
  startBle();
  audioReady = VoiceInput::begin();
  if (!audioReady) {
    Serial.println(F("[启动] I2S/录音文件系统初始化失败。BLE 配置仍保持可用。"));
    setState(DeviceState::Error);
  }
  previousButton = digitalRead(VoiceInput::PIN_VOICE_BUTTON);
  if (connectWifi()) {
    startPhoneWeb();
    selectPreferredServer();
  }
  publishStatus("ready");
  setState(audioReady ? DeviceState::Idle : DeviceState::Error);
  Serial.println(F("\nESP32-S3 云端语音设备已启动。"));
  Serial.println(F("按住 BOOT 说话，松开发送；播放时按 BOOT 停止。"));
  printStatus();
  if (audioReady) traceRuntime("idle", "设备就绪");
}

void loop() {
  using namespace CloudDevice;
  if (webStarted) phoneWeb.handleClient();
  processConsole();
  processBleRequests();
  processPendingNetworkActions();
  const bool button = digitalRead(VoiceInput::PIN_VOICE_BUTTON);
  if (!busy && previousButton == HIGH && button == LOW) {
    delay(28);
    if (digitalRead(VoiceInput::PIN_VOICE_BUTTON) == LOW) runDialogue();
  }
  previousButton = digitalRead(VoiceInput::PIN_VOICE_BUTTON);
  delay(8);
}
