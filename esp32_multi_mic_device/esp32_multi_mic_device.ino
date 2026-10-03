#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
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
#include <esp_heap_caps.h>

#include "alarm_manager.h"
#include "web_alarm_music.h"
#include "music_player.h"
#include "full_music_player.h"
#include "web_music_ui.h"
#include "bounded_audio_download.h"
#include "conversation_session.h"
#include "firmware_update.h"
#include "memo_manager.h"
#include "offline_wake.h"
#include "robot_controls.h"
#include "robot_display.h"
#include "robot_expression_catalog.h"
#include "robot_expression_web.h"
#include "robot_photo.h"
#include "robot_photo_ui.h"
#include "robot_servos.h"
#include "web_admin.h"
#include "web_admin_ui.h"
#include "web_display_ui.h"
#include "web_expressions_ui.h"
#include "wifi_profiles.h"
#include "wifi_scan.h"
#include "web_network_ui.h"
#include "voice_input.h"
#include "multi_mic_config.h"
#include "web_multi_mic_ui.h"
#include "voice_capture_settings.h"
#include "web_dialogue_ui.h"
#include "web_voice_ui.h"
#include "reminder_commands.h"
#include "web_memo_ui.h"
#include "web_usage_ui.h"
#include "web_response_stream.h"
#include "personal_profile.h"
#include "web_profile_ui.h"
#include "focus_timer.h"
#include "web_focus_ui.h"
#include "custom_wake.h"
#include "web_custom_wake.h"
#include "dialogue_expression.h"
#include "robot_interaction.h"
#include "web_interaction_ui.h"

#if !CONFIG_IDF_TARGET_ESP32S3 || !defined(ARDUINO_PARTITION_esp_sr_16)
#error "Select ESP32S3 Dev Module and Partition Scheme -> ESP SR 16M."
#endif
#if !defined(BOARD_HAS_PSRAM) || !CONFIG_SPIRAM_MODE_OCT
#error "Select PSRAM -> OPI PSRAM; the screen and offline wake models require it."
#endif
constexpr bool configuredFor16MbFlash() {
  constexpr char fqbn[] = ARDUINO_FQBN;
  constexpr char required[] = "FlashSize=16M";
  for (size_t i = 0; i + sizeof(required) <= sizeof(fqbn); ++i) {
    if (i && fqbn[i - 1] != ':' && fqbn[i - 1] != ',') continue;
    size_t j = 0;
    while (j < sizeof(required) - 1 && fqbn[i + j] == required[j]) ++j;
    if (j == sizeof(required) - 1 && (fqbn[i + j] == ',' || fqbn[i + j] == '\0')) return true;
  }
  return false;
}
static_assert(configuredFor16MbFlash(), "Select Flash Size -> 16MB; 4MB causes a boot loop with ESP SR 16M partitions.");

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// HTTPS certificate verification and OTA manifest signature handling use
// substantially more stack than the default Arduino loop task provides.
SET_LOOP_TASK_STACK_SIZE(16384);

#ifndef CLOUD_PROVISIONED_DEVICE_ID
#define CLOUD_PROVISIONED_DEVICE_ID ""
#endif
#ifndef CLOUD_PROVISIONED_DEVICE_TOKEN
#define CLOUD_PROVISIONED_DEVICE_TOKEN ""
#endif
#ifndef AMAP_PROVISIONED_WEB_KEY
#define AMAP_PROVISIONED_WEB_KEY ""
#endif

namespace CloudDevice {

constexpr uint32_t SERIAL_BAUD = 460800;
constexpr uint32_t WIFI_TIMEOUT_MS = 15000;
constexpr uint32_t WIFI_LOST_PORTAL_DELAY_MS = 30000;
constexpr uint32_t CONFIG_PORTAL_RETRY_MS = 60000;
constexpr uint32_t CONFIG_PORTAL_CLOSE_DELAY_MS = 15000;
constexpr uint32_t CONFIG_BUTTON_HOLD_MS = 5000;
constexpr uint16_t CONFIG_DNS_PORT = 53;
constexpr uint32_t RECORD_MINIMUM_MS = 400;
constexpr uint32_t RECORD_MAXIMUM_MS = 20000;
constexpr uint32_t HANDS_FREE_MINIMUM_MS = 800;
constexpr uint32_t HANDS_FREE_MAXIMUM_MS = 25000;
constexpr uint32_t JOB_TIMEOUT_MS = 120000;
constexpr uint32_t NTP_TIMEOUT_MS = 10000;
constexpr uint8_t OTA_NIGHT_HOUR = 3;
constexpr uint8_t OTA_NIGHT_WINDOW_MINUTES = 10;
constexpr uint32_t OTA_SCHEDULE_POLL_MS = 30000;
constexpr uint32_t WEATHER_FIRST_REFRESH_DELAY_MS = 8000;
constexpr uint32_t ERROR_DISPLAY_RETURN_MS = 8000;
constexpr uint32_t WEATHER_REFRESH_INTERVAL_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t WEATHER_RETRY_INTERVAL_MS = 15UL * 1000UL;
constexpr size_t RUNTIME_EVENT_COUNT = 24;
constexpr size_t RUNTIME_EVENT_LENGTH = 120;
constexpr size_t MAX_CONVERSATION_TEXT_BYTES = 1400;
constexpr size_t MAX_CONVERSATION_LOG_BYTES = 24576;
constexpr char CONVERSATION_LOG_PATH[] = "/conversation_history.jsonl";
uint32_t profileSyncedRevision = 0;
bool profileSyncConfirmed = false;
bool profileSyncRequested = true;
uint32_t nextProfileSyncMs = 0;
String profileSyncMessage = "等待联网同步";
String pendingFocusAnnouncement;
uint32_t lastFocusDisplayMs = 0;
constexpr char DEFAULT_SERVER_URL[] = "https://voice.bsnlch.xyz";
constexpr char FIRMWARE_VERSION[] = "2.1.0";
constexpr char OTA_MANIFEST_PATH[] = "/voice-api/v1/firmware/manifest";
constexpr char AMAP_API_HOST[] = "restapi.amap.com";
constexpr bool DIALOGUE_TRANSPORT_HTTP = true;
constexpr char RECORDING_PATH[] = "/cloud_question.pcm";
constexpr char CONFIG_AP_PASSWORD[] = "12345678";

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

// GlobalSign Root R3 validates the current restapi.amap.com certificate chain.
// It expires on 2029-03-18 and must be reviewed before that date.
static const char GLOBALSIGN_ROOT_R3[] PROGMEM = R"CERT(
-----BEGIN CERTIFICATE-----
MIIDXzCCAkegAwIBAgILBAAAAAABIVhTCKIwDQYJKoZIhvcNAQELBQAwTDEgMB4GA1UECxMXR2xv
YmFsU2lnbiBSb290IENBIC0gUjMxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkdsb2Jh
bFNpZ24wHhcNMDkwMzE4MTAwMDAwWhcNMjkwMzE4MTAwMDAwWjBMMSAwHgYDVQQLExdHbG9iYWxT
aWduIFJvb3QgQ0EgLSBSMzETMBEGA1UEChMKR2xvYmFsU2lnbjETMBEGA1UEAxMKR2xvYmFsU2ln
bjCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMwldpB5BngiFvXAg7aEyiie/QV2EcWt
iHL8RgJDx7KKnQRfJMsuS+FggkbhUqsMgUdwbN1k0ev1LKMPgj0MK66X17YUhhB5uzsTgHeMCOFJ
0mpiLx9e+pZo34knlTifBtc+ycsmWQ1z3rDI6SYOgxXG71uL0gRgykmmKPZpO/bLyCiR5Z2KYVc3
rHQU3HTgOu5yLy6c+9C7v/U9AOEGM+iCK65TpjoWc4zdQQ4gOsC0p6Hpsk+QLjJg6VfLuQSSaGjl
OCZgdbKfd/+RFO+uIEn8rUAVSNECMWEZXriX7613t2Saer9fwRPvm2L7DWzgVGkWqQPabumDk3F2
xmmFghcCAwEAAaNCMEAwDgYDVR0PAQH/BAQDAgEGMA8GA1UdEwEB/wQFMAMBAf8wHQYDVR0OBBYE
FI/wS3+oLkUkrk1Q+mOai97i3Ru8MA0GCSqGSIb3DQEBCwUAA4IBAQBLQNvAUKr+yAzv95ZURUm7
lgAJQayzE4aGKAczymvmdLm6AC2upArT9fHxD4q/c2dKg8dEe3jgr25sbwMpjjM5RcOO5LlXbKr8
EpbsU8Yt5CRsuZRj+9xTaGdWPoO4zzUhw8lo/s7awlOqzJCK6fBdRoyV3XpYKBovHd7NADdBj+1E
bddTKJd+82cEHhXXipa0095MJ6RMG3NzdvQXmcIfeg7jLQitChws/zyrVQ4PkX4268NXSb7hLi18
YIvDQVETI53O9zJrlAGomecsMx86OyXShkDOOyyGeMlhLxS67ttVb9+E7gUJTb0o2HLO02JQZR7r
kpeDMdmztcpHWD9f
-----END CERTIFICATE-----
)CERT";

enum class DeviceState : uint8_t { Booting, Idle, Recording, Processing, Playing, Error };

struct RuntimeEvent {
  uint32_t atMs;
  char text[RUNTIME_EVENT_LENGTH];
};

struct WeatherRequest {
  char city[65];
  char adcode[7];
  char apiKey[65];
  float latitude;
  float longitude;
  bool resolveLocation;
  bool locateByIp;
};

struct WeatherResult {
  bool success;
  bool locationResolved;
  bool locateByIp;
  char query[65];
  char city[65];
  char adcode[7];
  float latitude;
  float longitude;
  char aiContext[481];
};

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String serverUrl = DEFAULT_SERVER_URL;
  String deviceId;
  String deviceToken;
  uint32_t adminPin = 123456;
  uint8_t volumePercent = 50;
  uint8_t chineseSpeakerId = 0;
  uint16_t englishSpeakerId = 0;
  float speechSpeed = 1.0f;
  bool volumePotentiometer = false;
  bool otaAutoUpdate = false;
  bool offlineWakeEnabled = true;
  String offlineWakeModel = OfflineWake::DEFAULT_MODEL;
  uint8_t displayPages = RobotDisplay::DEFAULT_PAGES;
  String otaDeferredVersion;
  String amapKey;
  String weatherCity = "上海";
  String weatherAdcode = "310000";
  String weatherLocationMode = "manual";
  float weatherLatitude = 31.2304f;
  float weatherLongitude = 121.4737f;
};

Settings settings;
Preferences preferences;
bool busy = false;
bool pendingWebRecording = false;
bool manualRecordingActive = false;
ConversationSession conversation;
VoiceInput::AudioStats lastRecording;
bool audioReady = false;
bool webStarted = false;
bool configPortalActive = false;
bool pendingWifiReconnect = false;
bool preferConfiguredWifi = false;
bool pendingVoiceSync = false;
bool pendingOtaCheck = false;
bool pendingOtaInstall = false;
uint32_t displayReturnAtMs = 0;
WebServer phoneWeb(80);
DNSServer configDns;
SecureClient cloudSecureClient;
WiFiClient cloudPlainClient;
HTTPClient cloudHttp;
String activeServerUrl;
String cloudConnectionBaseUrl;
bool previousTalkPressed = false;
String serialLine;
bool clockReady = false;
uint32_t lastCloudActivityMs = 0;
String configApSsid;
uint32_t configPortalCloseAtMs = 0;
uint32_t nextConfigWifiRetryAtMs = 0;
uint32_t wifiDisconnectedSinceMs = 0;
uint32_t abortHeldSinceMs = 0;
bool abortLongPressHandled = false;
uint32_t pendingDisplayRestartAtMs = 0;
uint8_t webPinFailures = 0;
uint32_t webPinLockedUntilMs = 0;
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
uint32_t nextWeatherRefreshAtMs = 0;
uint32_t lastDisplayNetworkAtMs = 0;
uint32_t lastOtaSchedulePollAtMs = 0;
int lastOtaScheduleDateKey = -1;
QueueHandle_t weatherResultQueue = nullptr;
QueueHandle_t weatherRequestQueue = nullptr;
TaskHandle_t weatherWorkerHandle = nullptr;
bool weatherRefreshRunning = false;
bool weatherRefreshRequested = false;
bool weatherLocationNeedsResolution = true;
String weatherAiContext;
uint32_t weatherAiUpdatedAtMs = 0;
String currentUserText;
String currentAssistantText;
String historyAssistantText;
uint32_t currentUserAt = 0;
uint32_t currentAssistantAt = 0;
String otaStatus = "not_checked";
String otaMessage;
String otaAvailableVersion;
String lastLocalCommandJobId;
String lastLocalReportJobId;
bool localReportRequired = false;
ReminderCommands::Result currentLocalResult;
String lastPhotoCommandJobId;
String photoCommandMessage;
uint32_t lastAlarmToneAtMs = 0;
uint32_t alarmRingStartedAtMs = 0;
bool alarmTalkWasPressed = false;
File alarmMusicUpload;
size_t alarmMusicUploadBytes = 0;
bool alarmMusicUploadFailed = false;
String alarmMusicUploadTitle;

void startPhoneWeb();

void showStageOnDisplay(const char *stage, const String &message, bool error) {
  const String value(stage);
  if (error || value == "error" || value.endsWith("_error")) {
    RobotDisplay::show(RobotDisplay::Scene::Error, "出现问题", message);
  } else if (value == "booting") {
    RobotDisplay::show(RobotDisplay::Scene::Booting, "启动中", message);
  } else if (value.startsWith("wifi_") || value.startsWith("time_") ||
             value.startsWith("server_") || value == "checking_network") {
    RobotDisplay::show(RobotDisplay::Scene::Connecting, "正在连接", message);
  } else if (value == "recording") {
    RobotDisplay::show(RobotDisplay::Scene::Listening, "正在听", message);
  } else if (value == "playing") {
    RobotDisplay::show(RobotDisplay::Scene::Speaking, "AI 回答", currentAssistantText);
  } else if (value == "completed" || value == "idle" || value == "ota_complete") {
    RobotDisplay::show(RobotDisplay::Scene::Idle, "待机", "");
  } else if (value.startsWith("ota_")) {
    RobotDisplay::show(RobotDisplay::Scene::Updating, "固件升级", message);
  } else if (value == "uploading" || value == "upload_retry" ||
             value == "waiting_segment" || value.startsWith("audio_")) {
    RobotDisplay::show(RobotDisplay::Scene::Thinking, "思考中", currentUserText);
  }
}

void traceRuntime(const char *stage, const String &message = "", bool error = false) {
  dialogueStage = stage;
  stageStartedMs = millis();
  if (error) {
    lastDialogueError = message;
    displayReturnAtMs = millis() + ERROR_DISPLAY_RETURN_MS;
  } else {
    displayReturnAtMs = 0;
  }
  String line = String(stage);
  if (!message.isEmpty()) line += ": " + message;
  RuntimeEvent &event = runtimeEvents[runtimeEventHead];
  event.atMs = millis();
  strlcpy(event.text, line.c_str(), sizeof(event.text));
  runtimeEventHead = (runtimeEventHead + 1) % RUNTIME_EVENT_COUNT;
  runtimeEventSize = min(runtimeEventSize + 1, RUNTIME_EVENT_COUNT);
  Serial.printf("[FLOW][%s] %s\n", stage, message.c_str());
  showStageOnDisplay(stage, message, error);
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

void refreshMemoDisplay() {
  RobotDisplay::setMemoStatus(MemoManager::displaySummary(MemoManager::Kind::Note),
                              MemoManager::displaySummary(MemoManager::Kind::Todo),
                              MemoManager::count(MemoManager::Kind::Note),
                              MemoManager::count(MemoManager::Kind::Todo),
                              MemoManager::pendingTodoCount());
}

void returnToHomeDisplay(const String &message = "") {
  setState(DeviceState::Idle);
  RobotDisplay::clearConversation();
  currentAssistantText = "";
  historyAssistantText = "";
  RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
  refreshMemoDisplay();
  RobotDisplay::show(RobotDisplay::Scene::Idle, "待机", message);
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

String autoReturnPage(const String &title, const String &message, uint8_t delaySeconds) {
  String page;
  page.reserve(1800);
  page = F("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>"
           "<title>");
  page += htmlEscape(title);
  page += F("</title><style>body{font-family:system-ui;max-width:620px;margin:48px auto;padding:0 18px;"
            "color:#172033;background:#f4f7fb}.card{background:#fff;border:1px solid #d9e0ea;"
            "border-radius:12px;padding:22px}a{color:#1267d6}</style></head><body><div class='card'><h2>");
  page += htmlEscape(title);
  page += F("</h2><p>");
  page += htmlEscape(message);
  page += F("</p><p id='return-state'></p>"
            "<p><a href='/'>立即返回主页</a></p></div><script>let left=");
  page += String(delaySeconds);
  page += F(";const state=document.getElementById('return-state');async function go(){if(left>0){"
            "state.textContent=left+' 秒后自动返回主页…';left--;setTimeout(go,1000);return;}"
            "try{const r=await fetch('/status',{cache:'no-store'});if(r.ok){location.replace('/');return;}}"
            "catch(_){}state.textContent='设备正在处理或重新联网，恢复后将自动返回主页…';"
            "setTimeout(go,1500)}go();</script></body></html>");
  return page;
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

uint32_t conversationTimestamp() {
  const time_t now = time(nullptr);
  return now >= 1700000000 ? static_cast<uint32_t>(now) : 0;
}

void setCurrentAssistantText(const String &text) {
  if (!text.isEmpty() && !currentAssistantAt) currentAssistantAt = conversationTimestamp();
  currentAssistantText = utf8Clip(text, MAX_CONVERSATION_TEXT_BYTES);
}

void appendHistoryAssistantText(const String &text) {
  if (text.isEmpty()) return;
  if (!currentAssistantAt) currentAssistantAt = conversationTimestamp();
  if (!historyAssistantText.isEmpty()) historyAssistantText += " ";
  historyAssistantText += text;
  historyAssistantText = utf8Clip(historyAssistantText, MAX_CONVERSATION_TEXT_BYTES);
}

void saveConversationTurn() {
  if (currentUserText.isEmpty() && historyAssistantText.isEmpty()) return;
  if (!historyAssistantText.isEmpty() && !currentAssistantAt) currentAssistantAt = conversationTimestamp();
  fs::FS &storage = VoiceInput::recordingStorage();
  File probe = storage.open(CONVERSATION_LOG_PATH, FILE_READ);
  const size_t existing = probe ? probe.size() : 0;
  if (probe) probe.close();

  JsonDocument doc;
  doc["at_ms"] = millis();
  doc["user_at"] = currentUserAt;
  doc["assistant_at"] = currentAssistantAt;
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

String otaManifestUrl() {
  // OTA never inherits the dialogue transport's HTTP fallback.
  return baseUrlWithScheme(settings.serverUrl, true) + OTA_MANIFEST_PATH;
}

String baseFromRequestUrl(const String &url) {
  const int schemeEnd = url.indexOf("://");
  const int hostStart = schemeEnd >= 0 ? schemeEnd + 3 : 0;
  const int pathStart = url.indexOf('/', hostStart);
  if (pathStart < 0) return normalizedUrl(url);
  return normalizedUrl(url.substring(0, pathStart));
}

bool cloudMusicHeld = false;
bool alarmMusicHeld = false;
bool alarmPreview = false;
uint32_t alarmPreviewStarted = 0;
bool loadAlarmMusic();
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
  if (!cloudMusicHeld) { if (!FullMusic::hold()) return false; cloudMusicHeld = true; }
  const String baseUrl = baseFromRequestUrl(url);
  if (!cloudConnectionBaseUrl.isEmpty() && cloudConnectionBaseUrl != baseUrl) {
    closeCloudConnection();
  }

  cloudHttp.setReuse(true);
  if (url.startsWith("https://")) {
    if (!clockReady) { FullMusic::release(); cloudMusicHeld = false; return false; }
    cloudSecureClient.setCACert(DIGICERT_GLOBAL_ROOT_G2);
    cloudSecureClient.setHandshakeTimeout(8);
    cloudConnectionBaseUrl = baseUrl;
    const bool ok = cloudHttp.begin(cloudSecureClient, url);
    if (!ok) { FullMusic::release(); cloudMusicHeld = false; }
    return ok;
  }
  cloudConnectionBaseUrl = baseUrl;
  const bool ok = cloudHttp.begin(cloudPlainClient, url);
  if (!ok) { FullMusic::release(); cloudMusicHeld = false; }
  return ok;
}

void endCloudRequest(bool keepAlive = true) {
  cloudHttp.end();
  lastCloudActivityMs = millis();
  if (!keepAlive || cloudMusicHeld) closeCloudConnection();
  if (cloudMusicHeld) { FullMusic::release(); cloudMusicHeld = false; }
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
  settings.volumePotentiometer = legacyVolumePot && !RobotDisplay::BATTERY_MONITOR_ENABLED &&
                                 !RobotDisplay::potentiometerBrightnessEnabled();
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
  const bool adminPinV2 = preferences.getBool("pin_v2", false);
  const bool displayPagesV2 = preferences.isKey("disp_pages2");
  const bool legacyDisplayPagesStored = preferences.isKey("disp_pages") ||
                                        preferences.isKey("disp_cycle");
  settings.wifiSsid = preferences.getString("wifi_ssid", "");
  settings.wifiPassword = preferences.getString("wifi_pass", "");
  settings.serverUrl = preferences.getString("server_url", DEFAULT_SERVER_URL);
  settings.deviceId = preferences.getString("device_id", hardwareDeviceId());
  settings.deviceToken = preferences.getString("device_token", "");
  settings.adminPin = adminPinV2 ? preferences.getUInt("admin_pin", 123456) : 123456;
  settings.volumePercent = preferences.getUChar("volume", 50);
  settings.chineseSpeakerId = preferences.getUChar("spk_zh", 0);
  settings.englishSpeakerId = preferences.getUShort("spk_en", 0);
  settings.speechSpeed = preferences.getFloat("tts_speed", 1.0f);
  settings.volumePotentiometer = preferences.getBool("volume_pot", false);
  settings.otaAutoUpdate = preferences.getBool("ota_auto", false);
  settings.offlineWakeEnabled = preferences.getBool("wake_on", true);
  settings.offlineWakeModel = preferences.getString(
      "wake_model", OfflineWake::DEFAULT_MODEL);
  const uint8_t legacyDisplayPages = preferences.getUChar(
      "disp_pages", preferences.getBool("disp_cycle", true) ? 0x03 : 0x01);
  settings.displayPages = displayPagesV2
                              ? preferences.getUChar("disp_pages2", RobotDisplay::DEFAULT_PAGES)
                              : (legacyDisplayPagesStored ? legacyDisplayPages : RobotDisplay::DEFAULT_PAGES);
  settings.displayPages &= RobotDisplay::PAGE_ALL;
  if (settings.displayPages == 0) settings.displayPages = RobotDisplay::PAGE_HOME;
  settings.otaDeferredVersion = preferences.getString("ota_defer", "");
  settings.amapKey = preferences.getString("amap_key", "");
  settings.weatherCity = preferences.getString("wx_city", "上海");
  settings.weatherAdcode = preferences.getString("wx_adcode", "310000");
  settings.weatherLocationMode = preferences.getString("wx_mode", "manual");
  settings.weatherLatitude = preferences.getFloat("wx_lat", 31.2304f);
  settings.weatherLongitude = preferences.getFloat("wx_lon", 121.4737f);
  preferences.end();
  if (!displayPagesV2) {
    preferences.begin("cloud-ai", false);
    const bool stored = preferences.putUChar("disp_pages2", settings.displayPages) > 0;
    preferences.end();
    Serial.println(stored ? F("[显示] 已保留原待机页面选择；可在网页中选择表情、便签和照片页。")
                          : F("[显示] 待机页面选择迁移写入失败，本次仍按原选择显示。"));
  }
  if (!adminPinV2 || settings.adminPin < 100000 || settings.adminPin > 999999) {
    settings.adminPin = 123456;
    preferences.begin("cloud-ai", false);
    const bool stored = preferences.putUInt("admin_pin", settings.adminPin) > 0 &&
                        preferences.putBool("pin_v2", true);
    preferences.end();
    Serial.println(stored ? F("[配置] 设备连接码已初始化为默认值 123456；以后不会随机变化。")
                          : F("[配置] 默认设备连接码已生效，但写入 NVS 失败。"));
  }
  if (settings.deviceToken.isEmpty() && strlen(CLOUD_PROVISIONED_DEVICE_TOKEN) > 0) {
    settings.deviceId = CLOUD_PROVISIONED_DEVICE_ID;
    settings.deviceToken = CLOUD_PROVISIONED_DEVICE_TOKEN;
    Serial.println(F("[配置] 已载入本机 secrets.h 的设备凭证；Token 不会输出到串口或网页。"));
  }
  if (settings.amapKey.isEmpty() && strlen(AMAP_PROVISIONED_WEB_KEY) > 0) {
    settings.amapKey = AMAP_PROVISIONED_WEB_KEY;
    preferences.begin("cloud-ai", false);
    const bool stored = preferences.putString("amap_key", settings.amapKey) > 0;
    preferences.end();
    Serial.println(stored ? F("[天气] 已将本机 secrets.h 的高德 Key 写入 NVS；Key 不会输出。")
                          : F("[天气] 高德 Key 已载入，但写入 NVS 失败。"));
  }
  migrateLegacyLanSettings();
  if (!WifiProfiles::begin(settings.wifiSsid, settings.wifiPassword)) {
    Serial.println(F("[Wi-Fi] 已保存网络列表读取失败，请在后台重新保存网络。"));
  }
  settings.serverUrl = normalizedUrl(settings.serverUrl);
  settings.volumePercent = constrain(settings.volumePercent, 0, 100);
  settings.chineseSpeakerId = constrain(settings.chineseSpeakerId, 0, 4);
  settings.englishSpeakerId = constrain(settings.englishSpeakerId, 0, 903);
  settings.speechSpeed = constrain(settings.speechSpeed, 0.5f, 2.0f);
  if (!OfflineWake::isSupportedModel(settings.offlineWakeModel)) {
    settings.offlineWakeModel = OfflineWake::DEFAULT_MODEL;
  }
  if (settings.weatherCity.isEmpty()) settings.weatherCity = "上海";
  if (settings.weatherLocationMode != "ip") settings.weatherLocationMode = "manual";
  settings.weatherLatitude = constrain(settings.weatherLatitude, -90.0f, 90.0f);
  settings.weatherLongitude = constrain(settings.weatherLongitude, -180.0f, 180.0f);
  weatherLocationNeedsResolution = settings.weatherLocationMode == "ip" ||
                                   settings.weatherAdcode.length() != 6;
  if (RobotDisplay::BATTERY_MONITOR_ENABLED || RobotDisplay::potentiometerBrightnessEnabled())
    settings.volumePotentiometer = false;
  VoiceInput::setPlaybackVolume(settings.volumePercent);
  VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
  RobotDisplay::setPageMask(settings.displayPages);
}

bool saveSettings() {
  preferences.begin("cloud-ai", false);
  bool ok = true;
  ok &= preferences.putString("wifi_ssid", settings.wifiSsid) > 0 || settings.wifiSsid.isEmpty();
  ok &= preferences.putString("wifi_pass", settings.wifiPassword) > 0 || settings.wifiPassword.isEmpty();
  ok &= preferences.putString("server_url", settings.serverUrl) > 0;
  ok &= preferences.putString("device_id", settings.deviceId) > 0;
  ok &= preferences.putString("device_token", settings.deviceToken) > 0 || settings.deviceToken.isEmpty();
  ok &= preferences.putUInt("admin_pin", settings.adminPin) > 0;
  ok &= preferences.putBool("pin_v2", true);
  ok &= preferences.putUChar("volume", settings.volumePercent) > 0;
  ok &= preferences.putUChar("spk_zh", settings.chineseSpeakerId) > 0 || settings.chineseSpeakerId == 0;
  ok &= preferences.putUShort("spk_en", settings.englishSpeakerId) > 0 || settings.englishSpeakerId == 0;
  ok &= preferences.putFloat("tts_speed", settings.speechSpeed) > 0;
  ok &= preferences.putBool("volume_pot", settings.volumePotentiometer) > 0;
  ok &= preferences.putBool("ota_auto", settings.otaAutoUpdate) > 0 || !settings.otaAutoUpdate;
  ok &= preferences.putBool("wake_on", settings.offlineWakeEnabled) > 0 ||
        !settings.offlineWakeEnabled;
  ok &= preferences.putString("wake_model", settings.offlineWakeModel) > 0;
  ok &= preferences.putUChar("disp_pages", settings.displayPages) > 0;
  ok &= preferences.putUChar("disp_pages2", settings.displayPages) > 0;
  ok &= preferences.putString("ota_defer", settings.otaDeferredVersion) > 0 ||
        settings.otaDeferredVersion.isEmpty();
  ok &= preferences.putString("amap_key", settings.amapKey) > 0 || settings.amapKey.isEmpty();
  ok &= preferences.putString("wx_city", settings.weatherCity) > 0;
  ok &= preferences.putString("wx_adcode", settings.weatherAdcode) > 0 ||
        settings.weatherAdcode.isEmpty();
  ok &= preferences.putString("wx_mode", settings.weatherLocationMode) > 0;
  ok &= preferences.putFloat("wx_lat", settings.weatherLatitude) > 0 || settings.weatherLatitude == 0.0f;
  ok &= preferences.putFloat("wx_lon", settings.weatherLongitude) > 0 || settings.weatherLongitude == 0.0f;
  preferences.end();
  return ok;
}

String configPortalUrl() {
  return "http://" + WiFi.softAPIP().toString() + "/setup";
}

void updateProvisioningDisplay() {
  RobotDisplay::setProvisioning(configPortalActive, configApSsid, CONFIG_AP_PASSWORD,
                               configPortalUrl(), WiFi.softAPgetStationNum() > 0);
}

bool startConfigPortal(const String &reason) {
  if (configPortalActive) {
    updateProvisioningDisplay();
    displayReturnAtMs = 0;
    RobotDisplay::show(RobotDisplay::Scene::Connecting, "Wi-Fi 配网",
                       configApSsid + "\n" + WiFi.softAPIP().toString());
    return true;
  }
  char name[24];
  snprintf(name, sizeof(name), "ROBOT-%04X",
           static_cast<uint16_t>(ESP.getEfuseMac()));
  configApSsid = name;
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!WiFi.softAP(configApSsid.c_str(), CONFIG_AP_PASSWORD)) {
    Serial.println(F("[配网] 无法启动 Wi-Fi 配置热点。"));
    traceRuntime("config_portal_error", "配置热点启动失败", true);
    return false;
  }
  configPortalActive = true;
  updateProvisioningDisplay();
  configPortalCloseAtMs = 0;
  nextConfigWifiRetryAtMs = millis() + CONFIG_PORTAL_RETRY_MS;
  wifiDisconnectedSinceMs = 0;
  displayReturnAtMs = 0;
  configDns.setErrorReplyCode(DNSReplyCode::NoError);
  configDns.start(CONFIG_DNS_PORT, "*", WiFi.softAPIP());
  startPhoneWeb();
  Serial.printf("[配网] %s；热点=%s，密码=%s，地址=%s\n", reason.c_str(),
                configApSsid.c_str(), CONFIG_AP_PASSWORD,
                configPortalUrl().c_str());
  traceRuntime("config_portal", reason + "，连接 " + configApSsid);
  RobotDisplay::show(RobotDisplay::Scene::Connecting, "Wi-Fi 配网",
                     configApSsid + "\n" + WiFi.softAPIP().toString());
  return true;
}

void stopConfigPortal() {
  if (!configPortalActive) return;
  configDns.stop();
  WiFi.softAPdisconnect(true);
  configPortalActive = false;
  RobotDisplay::setProvisioning(false);
  configPortalCloseAtMs = 0;
  nextConfigWifiRetryAtMs = 0;
  if (WiFi.status() == WL_CONNECTED) WiFi.mode(WIFI_STA);
  Serial.println(F("[配网] 配置热点已关闭。"));
}

bool connectWifi(bool force = false) {
  if (WiFi.status() == WL_CONNECTED && !force) return true;
  // We control retries from fresh scan results, not the driver's last SSID.
  WiFi.setAutoReconnect(false);
  if (WifiProfiles::count() == 0) {
    preferConfiguredWifi = false;
    traceRuntime("wifi_error", "尚未保存 Wi-Fi，请在配置热点后台选择网络", true);
    return false;
  }
  struct BusyGuard {
    bool previous = busy;
    BusyGuard() { busy = true; }
    ~BusyGuard() { busy = previous; }
  } busyGuard;
  auto serviceWait = []() {
    if (configPortalActive) configDns.processNextRequest();
    if (webStarted) phoneWeb.handleClient();
    delay(20);
  };
  WiFi.persistent(false);
  WiFi.mode(configPortalActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.disconnect(false, false);
  delay(100);
  closeCloudConnection();
  activeServerUrl = "";
  clockReady = false;
  Serial.printf("[Wi-Fi] 扫描附近网络；已保存 %u 组。\n", static_cast<unsigned>(WifiProfiles::count()));
  traceRuntime("wifi_scanning", "扫描附近 Wi-Fi，只连接扫描到的已保存网络");
  RobotDisplay::show(RobotDisplay::Scene::Connecting, "查找 Wi-Fi", "正在扫描附近网络");
  WifiScan::start(true);
  const uint32_t scanStarted = millis();
  while (WifiScan::running() && millis() - scanStarted < 10000) {
    WifiScan::poll();
    serviceWait();
  }
  if (WifiScan::running()) WifiScan::cancel();
  if (WifiScan::failed()) {
    preferConfiguredWifi = false;
    Serial.println(F("[Wi-Fi] 扫描失败，本轮不尝试历史网络。"));
    traceRuntime("wifi_scan_error", "扫描失败，稍后重新扫描", true);
    return false;
  }

  uint8_t order[WifiProfiles::MAX_NETWORKS] = {};
  const size_t candidates = WifiScan::connectionOrder(order, WifiProfiles::MAX_NETWORKS,
      preferConfiguredWifi ? settings.wifiSsid.c_str() : nullptr);
  preferConfiguredWifi = false;
  Serial.printf("[Wi-Fi] 扫描完成，可见的已保存网络 %u 组。\n", static_cast<unsigned>(candidates));
  if (candidates == 0) {
    Serial.println(F("[Wi-Fi] 附近没有已保存网络，进入配置热点，稍后重新扫描。"));
    traceRuntime("wifi_not_found", "附近没有已保存网络，请连接配置热点选择 Wi-Fi");
    return false;
  }
  for (size_t c = 0; c < candidates; ++c) {
    const auto profile = *WifiProfiles::get(order[c]);
    WiFi.disconnect(false, false);
    delay(100);
    Serial.printf("[Wi-Fi] 连接可见的已保存网络 %s（%u/%u）\n", profile.ssid,
                  static_cast<unsigned>(c + 1), static_cast<unsigned>(candidates));
    traceRuntime("wifi_connecting", String("正在连接 ") + profile.ssid);
    RobotDisplay::show(RobotDisplay::Scene::Connecting, "连接 Wi-Fi", profile.ssid);
    RobotDisplay::setNetwork(false, -127, configPortalActive ? WiFi.softAPIP().toString() : "");
    WiFi.begin(profile.ssid, profile.password);
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
      const auto status = WiFi.status();
      if (status == WL_NO_SSID_AVAIL || status == WL_CONNECT_FAILED) break;
      serviceWait();
    }
    if (WiFi.status() == WL_CONNECTED) {
      settings.wifiSsid = profile.ssid;
      settings.wifiPassword = profile.password;
      break;
    }
    Serial.printf("[Wi-Fi] 当前网络连接失败，状态=%d；停止本次连接。\n", static_cast<int>(WiFi.status()));
    WiFi.disconnect(false, false);
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) {
    traceRuntime("wifi_error", "附近已保存网络均未连接，配置热点保持可用", true);
    return false;
  }
  RobotDisplay::setNetwork(true, WiFi.RSSI(), WiFi.localIP().toString());
  Serial.printf("[Wi-Fi] 已连接，IP=%s，RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  traceRuntime("wifi_connected", "IP=" + WiFi.localIP().toString() + ", RSSI=" + String(WiFi.RSSI()));
  wifiDisconnectedSinceMs = 0;
  if (configPortalActive) {
    configPortalCloseAtMs = millis() + CONFIG_PORTAL_CLOSE_DELAY_MS;
    Serial.printf("[配网] 已连入目标 Wi-Fi，配置热点将在 %lu 秒后关闭。\n",
                  static_cast<unsigned long>(CONFIG_PORTAL_CLOSE_DELAY_MS / 1000));
  }

  configTzTime("CST-8", "ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org");
  const uint32_t ntpStarted = millis();
  while (time(nullptr) < 1700000000 && millis() - ntpStarted < NTP_TIMEOUT_MS) serviceWait();
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
  doc["hardware_sample_rate"] = VoiceInput::hardwareSampleRate();
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "";
  doc["server_url"] = settings.serverUrl;
  doc["active_server_url"] = activeServerUrl;
  doc["dialogue_base_url"] = dialogueBaseUrl();
  doc["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : settings.wifiSsid;
  doc["saved_wifi_count"] = WifiProfiles::count();
  doc["phone_url"] = WiFi.status() == WL_CONNECTED
                         ? "http://" + WiFi.localIP().toString() + "/"
                         : configPortalActive ? configPortalUrl() : "";
  doc["config_portal"] = configPortalActive;
  doc["config_ap_ssid"] = configPortalActive ? configApSsid : "";
  doc["config_ap_ip"] = configPortalActive ? WiFi.softAPIP().toString() : "";
  doc["config_ap_clients"] = configPortalActive ? WiFi.softAPgetStationNum() : 0;
  doc["tls_ca_verified"] = activeServerUrl.startsWith("https://");
  doc["clock_ready"] = clockReady;
  doc["device_id"] = settings.deviceId;
  doc["token_configured"] = !settings.deviceToken.isEmpty();
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["firmware_hardware"] = FirmwareUpdate::HARDWARE_ID;
  doc["ota_status"] = otaStatus;
  doc["ota_message"] = otaMessage;
  doc["ota_available_version"] = otaAvailableVersion;
  doc["ota_auto_update"] = settings.otaAutoUpdate;
  doc["ota_deferred_version"] = settings.otaDeferredVersion;
  doc["ota_manifest_url"] = otaManifestUrl();
  doc["amap_key_configured"] = !settings.amapKey.isEmpty();
  doc["weather_city"] = settings.weatherCity;
  doc["weather_adcode"] = settings.weatherAdcode;
  doc["weather_location_mode"] = settings.weatherLocationMode;
  doc["weather_latitude"] = settings.weatherLatitude;
  doc["weather_longitude"] = settings.weatherLongitude;
  doc["weather_ai_context_ready"] = !weatherAiContext.isEmpty();
  doc["weather_ai_context_age_minutes"] = weatherAiContext.isEmpty()
                                                ? -1
                                                : static_cast<int32_t>((millis() - weatherAiUpdatedAtMs) / 60000UL);
  doc["display_pages"] = settings.displayPages;
  doc["display_rotation"] = RobotDisplay::currentRotation() * 90;
  doc["display_brightness"] = RobotDisplay::brightnessSetting();
  doc["display_brightness_effective"] = RobotDisplay::effectiveBrightness();
  doc["display_brightness_mode"] = RobotDisplay::potentiometerBrightnessEnabled() ? "pot" : "fixed";
  doc["display_brightness_method"] = RobotDisplay::hardwareBacklightControl() ? "pwm" : "software";
  doc["display_button_layout"] = RobotDisplay::voiceFullscreen(false) ? "full" : "compact";
  doc["display_wake_layout"] = RobotDisplay::voiceFullscreen(true) ? "full" : "compact";
  doc["display_width"] = RobotDisplay::width();
  doc["display_height"] = RobotDisplay::height();
  doc["display_carousel"] = (settings.displayPages & (settings.displayPages - 1U)) != 0;
  doc["alarm_count"] = AlarmManager::count();
  doc["alarm_ringing"] = AlarmManager::ringing();
  doc["alarm_music_status"] = MusicPlayer::status();
  doc["ambient_noise_ready"] = VoiceInput::ambientNoiseReady();
  doc["ambient_noise_floor"] = VoiceInput::ambientNoiseFloor();
  doc["ambient_speech_threshold"] = VoiceInput::ambientSpeechThreshold();
  doc["microphone_advice"] = lastRecording.samples && lastRecording.clippedSamples > lastRecording.samples / 100
      ? "增益过高：请降低录音增益" : VoiceInput::ambientNoiseFloor() * VoiceCapture::settings().gain > 250
      ? "背景偏吵：请远离噪声源或打开降噪" : lastRecording.samples && !lastRecording.speechDetected
      ? "声音偏小：靠近麦克风或适当增加增益" : VoiceInput::ambientNoiseReady() ? "收音环境正常" : "正在校准底噪，请安静片刻";
  doc["next_alarm"] = AlarmManager::nextSummary(time(nullptr));
  doc["memo_count"] = MemoManager::count(MemoManager::Kind::Note);
  doc["todo_count"] = MemoManager::count(MemoManager::Kind::Todo);
  doc["pending_todo_count"] = MemoManager::pendingTodoCount();
  doc["battery_voltage"] = RobotDisplay::batteryVoltage();
  doc["battery_percent"] = RobotDisplay::batteryPercent();
  doc["volume_percent"] = settings.volumePercent;
  doc["chinese_speaker_id"] = settings.chineseSpeakerId;
  doc["english_speaker_id"] = settings.englishSpeakerId;
  doc["speech_speed"] = settings.speechSpeed;
  doc["volume_mode"] = settings.volumePotentiometer ? "pot" : "fixed";
  VoiceCapture::appendStatus(doc.as<JsonObject>());
  doc["conversation_active"] = conversation.active();
  doc["manual_recording_pending"] = pendingWebRecording;
  doc["manual_recording_active"] = manualRecordingActive;
  doc["last_recording_rms"] = lastRecording.rms;
  doc["last_recording_peak"] = lastRecording.peak;
  doc["last_recording_speech_ms"] = lastRecording.speechMs;
  doc["last_recording_clipped_samples"] = lastRecording.clippedSamples;
  doc["last_recording_noise_suppression"] = lastRecording.noiseSuppressionApplied;
  doc["last_recording_audio_ms"] = lastRecording.samples * 1000U / VoiceInput::SAMPLE_RATE;
  doc["last_recording_capture_ms"] = lastRecording.durationMs;
  doc["last_recording_preparation_ms"] = lastRecording.preparationMs;
  doc["last_recording_storage_ms"] = lastRecording.storageMs;
  doc["recording_backend"] = "PSRAM";
  doc["last_recording_storage_bytes_written"] = lastRecording.storageBytesWritten;
  doc["last_recording_storage_errno"] = lastRecording.storageError;
  doc["last_recording_web_ms"] = lastRecording.webServiceUs / 1000U;
  doc["last_recording_dsp_ms"] = lastRecording.processingUs / 1000U;
  doc["last_recording_max_read_gap_us"] = lastRecording.maxReadGapUs;
  doc["offline_wake_enabled"] = settings.offlineWakeEnabled;
  doc["offline_wake_ready"] = OfflineWake::ready();
  doc["offline_wake_listening"] = OfflineWake::listening();
  doc["offline_wake_model"] = settings.offlineWakeModel;
  doc["offline_wake_word"] = OfflineWake::wakeWord();
  doc["offline_wake_error"] = OfflineWake::lastError();
  doc["audio_ready"] = VoiceInput::ready();
  doc["display_ready"] = RobotDisplay::ready();
  doc["servos_ready"] = RobotServos::ready();
  doc["display_panel"] = RobotDisplay::panelId(RobotDisplay::currentPanel());
  doc["display_panel_label"] = RobotDisplay::panelLabel(RobotDisplay::currentPanel());
  doc["display_backlight_gpio"] = RobotDisplay::PIN_TFT_BACKLIGHT;
  doc["display_backlight_level"] = RobotDisplay::hardwareBacklightControl()
                                        ? (RobotDisplay::effectiveBrightness() * 255U + 50U) / 100U
                                        : -1;
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
  doc["current_user_at"] = currentUserAt;
  doc["current_assistant_at"] = currentAssistantAt;
  doc["firmware_version"] = FIRMWARE_VERSION;
  doc["ota_status"] = otaStatus;
  doc["ota_message"] = otaMessage;
  doc["ota_available_version"] = otaAvailableVersion;
  doc["ota_auto_update"] = settings.otaAutoUpdate;
  doc["ota_deferred_version"] = settings.otaDeferredVersion;
  doc["offline_wake_enabled"] = settings.offlineWakeEnabled;
  doc["offline_wake_ready"] = OfflineWake::ready();
  doc["offline_wake_listening"] = OfflineWake::listening();
  doc["offline_wake_word"] = OfflineWake::wakeWord();
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
  // The local status and runtime endpoints are polled by the web UI. Keep this
  // lightweight hook so existing call sites do not need transport-specific code.
  (void)message;
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

bool resolveWeatherHost(const char *host, IPAddress &address) {
  Serial.printf("[天气] 当前 DNS：%s / %s\n", WiFi.dnsIP(0).toString().c_str(),
                WiFi.dnsIP(1).toString().c_str());
  if (WiFi.hostByName(host, address) == 1) return true;

  struct DnsFallback {
    IPAddress primary;
    IPAddress secondary;
    const char *name;
  };
  const DnsFallback fallbacks[] = {
      {IPAddress(223, 5, 5, 5), IPAddress(119, 29, 29, 29), "AliDNS/Tencent DNS"},
      {IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8), "Cloudflare/Google DNS"},
  };
  for (const DnsFallback &fallback : fallbacks) {
    Serial.printf("[天气] DHCP DNS 无法解析，切换 %s：%s / %s\n", fallback.name,
                  fallback.primary.toString().c_str(), fallback.secondary.toString().c_str());
    if (!WiFi.setDNS(fallback.primary, fallback.secondary)) continue;
    delay(120);
    if (WiFi.hostByName(host, address) == 1) return true;
  }
  return false;
}

String urlEncodeComponent(const String &value) {
  constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += HEX_DIGITS[c >> 4];
      encoded += HEX_DIGITS[c & 0x0F];
    }
  }
  return encoded;
}

bool validAdcode(const String &value) {
  if (value.length() != 6) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
  }
  return true;
}

String jsonScalarText(JsonVariantConst value) {
  if (value.is<const char *>()) return value.as<String>();
  if (value.is<long>()) return String(value.as<long>());
  if (value.is<unsigned long>()) return String(value.as<unsigned long>());
  return "";
}

bool parseAmapLocation(const String &value, float &latitude, float &longitude) {
  const int comma = value.indexOf(',');
  if (comma <= 0 || comma >= static_cast<int>(value.length()) - 1) return false;
  longitude = value.substring(0, comma).toFloat();
  latitude = value.substring(comma + 1).toFloat();
  return latitude >= -90.0f && latitude <= 90.0f &&
         longitude >= -180.0f && longitude <= 180.0f;
}

bool parseAmapRectangleCenter(const String &value, float &latitude, float &longitude) {
  const int semicolon = value.indexOf(';');
  if (semicolon <= 0) return false;
  float firstLatitude = 0.0f;
  float firstLongitude = 0.0f;
  float secondLatitude = 0.0f;
  float secondLongitude = 0.0f;
  if (!parseAmapLocation(value.substring(0, semicolon), firstLatitude, firstLongitude) ||
      !parseAmapLocation(value.substring(semicolon + 1), secondLatitude, secondLongitude)) {
    return false;
  }
  latitude = (firstLatitude + secondLatitude) * 0.5f;
  longitude = (firstLongitude + secondLongitude) * 0.5f;
  return true;
}

bool amapApiGet(const String &path, const String &apiKey, JsonDocument &doc,
                String &errorMessage, size_t maximumBytes = 12288) {
  if (WiFi.status() != WL_CONNECTED) {
    errorMessage = "Wi-Fi未连接";
    return false;
  }
  if (apiKey.isEmpty()) {
    errorMessage = "未配置高德Key";
    return false;
  }
  IPAddress address;
  if (!resolveWeatherHost(AMAP_API_HOST, address)) {
    errorMessage = "高德DNS失败";
    return false;
  }
  Serial.printf("[天气] DNS %s -> %s\n", AMAP_API_HOST, address.toString().c_str());

  SecureClient client;
  if (!clockReady) {
    errorMessage = "系统时间未同步";
    return false;
  }
  client.setCACert(GLOBALSIGN_ROOT_R3);
  client.setHandshakeTimeout(8);
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const String separator = path.indexOf('?') >= 0 ? "&" : "?";
  const String url = String("https://") + AMAP_API_HOST + path + separator +
                     "key=" + urlEncodeComponent(apiKey);
  if (!http.begin(client, url)) {
    errorMessage = "高德HTTPS初始化失败";
    return false;
  }
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("User-Agent", String("ESP32-Robot/") + FIRMWARE_VERSION);
  const int code = http.GET();
  const String payload = code == HTTP_CODE_OK ? http.getString() : "";
  http.end();
  client.stop();
  if (code != HTTP_CODE_OK) {
    errorMessage = code < 0 ? HTTPClient::errorToString(code) : "高德HTTP " + String(code);
    return false;
  }
  if (payload.isEmpty() || payload.length() > maximumBytes) {
    errorMessage = "高德响应长度异常";
    return false;
  }
  const DeserializationError jsonError = deserializeJson(doc, payload);
  if (jsonError) {
    errorMessage = "高德JSON无效";
    return false;
  }
  if (jsonScalarText(doc["status"]) != "1") {
    errorMessage = jsonScalarText(doc["info"]);
    if (errorMessage.isEmpty()) errorMessage = "高德接口拒绝请求";
    return false;
  }
  return true;
}

bool resolveWeatherLocation(const String &query, const String &apiKey, bool locateByIp,
                            String &resolvedCity, String &adcode,
                            float &latitude, float &longitude) {
  RobotDisplay::setWeatherStatus(query, locateByIp ? "正在按IP定位" : "正在解析地址");
  JsonDocument doc;
  String errorMessage;
  const String path = locateByIp
                          ? "/v3/ip?output=JSON"
                          : "/v3/assistant/inputtips?output=JSON&datatype=all&keywords=" +
                                urlEncodeComponent(query);
  if (!amapApiGet(path, apiKey, doc, errorMessage)) {
    Serial.printf("[天气] 高德定位失败：%s。\n", errorMessage.c_str());
    RobotDisplay::setWeatherStatus(query, errorMessage);
    return false;
  }

  if (locateByIp) {
    const String foundAdcode = jsonScalarText(doc["adcode"]);
    const String foundCity = jsonScalarText(doc["city"]);
    if (!validAdcode(foundAdcode) || foundCity.isEmpty()) {
      RobotDisplay::setWeatherStatus(query, "IP无法定位");
      Serial.println(F("[天气] 高德 IP 定位未返回国内城市。"));
      return false;
    }
    adcode = foundAdcode;
    resolvedCity = foundCity;
    parseAmapRectangleCenter(jsonScalarText(doc["rectangle"]), latitude, longitude);
  } else {
    JsonArrayConst tips = doc["tips"].as<JsonArrayConst>();
    bool found = false;
    for (JsonObjectConst tip : tips) {
      const String foundAdcode = jsonScalarText(tip["adcode"]);
      if (!validAdcode(foundAdcode)) continue;
      adcode = foundAdcode;
      resolvedCity = jsonScalarText(tip["name"]);
      if (resolvedCity.isEmpty()) resolvedCity = query;
      parseAmapLocation(jsonScalarText(tip["location"]), latitude, longitude);
      found = true;
      break;
    }
    if (!found) {
      RobotDisplay::setWeatherStatus(query, "未找到该地址");
      Serial.printf("[天气] 高德未找到地址“%s”。\n", query.c_str());
      return false;
    }
  }
  Serial.printf("[天气] 高德定位“%s”，adcode=%s，坐标=%.4f, %.4f。\n",
                resolvedCity.c_str(), adcode.c_str(), latitude, longitude);
  return true;
}

bool refreshWeather(const String &city, const String &adcode, const String &apiKey,
                    WeatherResult &result) {
  if (WiFi.status() != WL_CONNECTED) return false;
  if (!validAdcode(adcode)) {
    RobotDisplay::setWeatherStatus(city, "地址编码无效");
    return false;
  }
  RobotDisplay::setWeatherStatus(city, "高德天气连接中");
  JsonDocument doc;
  String errorMessage;
  const String path = "/v3/weather/weatherInfo?output=JSON&extensions=base&city=" + adcode;
  if (!amapApiGet(path, apiKey, doc, errorMessage, 6144)) {
    Serial.printf("[天气] 高德天气失败：%s，15 秒后重试。\n", errorMessage.c_str());
    RobotDisplay::setWeatherStatus(city, errorMessage);
    return false;
  }
  JsonVariantConst live = doc["lives"][0];
  const String temperatureText = jsonScalarText(live["temperature"]);
  const String condition = jsonScalarText(live["weather"]);
  if (live.isNull() || temperatureText.isEmpty() || condition.isEmpty()) {
    Serial.println(F("[天气] 高德实况字段缺失，15 秒后重试。"));
    RobotDisplay::setWeatherStatus(city, "天气数据无效");
    return false;
  }
  const float temperature = temperatureText.toFloat();
  RobotDisplay::setWeather(city, temperature, condition, true);
  JsonDocument forecastDoc;
  const String forecastPath =
      "/v3/weather/weatherInfo?output=JSON&extensions=all&city=" + adcode;
  if (!amapApiGet(forecastPath, apiKey, forecastDoc, errorMessage, 12288)) {
    Serial.printf("[天气] 实况成功，但3天预报失败：%s，15 秒后重试。\n", errorMessage.c_str());
    RobotDisplay::setWeatherStatus(city, "预报更新失败");
    return false;
  }
  String dayLabels[3];
  String conditions[3];
  int highC[3] = {0, 0, 0};
  int lowC[3] = {0, 0, 0};
  size_t forecastCount = 0;
  for (JsonObjectConst cast : forecastDoc["forecasts"][0]["casts"].as<JsonArrayConst>()) {
    if (forecastCount >= 3) break;
    String date = jsonScalarText(cast["date"]);
    dayLabels[forecastCount] = forecastCount == 0
                                   ? "今天"
                                   : (date.length() >= 10 ? date.substring(5) : date);
    conditions[forecastCount] = jsonScalarText(cast["dayweather"]);
    const String night = jsonScalarText(cast["nightweather"]);
    if (!night.isEmpty() && night != conditions[forecastCount]) {
      conditions[forecastCount] += "/" + night;
    }
    highC[forecastCount] = jsonScalarText(cast["daytemp"]).toInt();
    lowC[forecastCount] = jsonScalarText(cast["nighttemp"]).toInt();
    if (!conditions[forecastCount].isEmpty()) ++forecastCount;
  }
  if (forecastCount < 2) {
    Serial.println(F("[天气] 高德预报字段不足，15 秒后重试。"));
    RobotDisplay::setWeatherStatus(city, "预报数据无效");
    return false;
  }
  RobotDisplay::setForecast(dayLabels, conditions, highC, lowC, forecastCount);
  String aiContext = "地点：" + city + "；当前：" + condition + " " +
                     temperatureText + "℃；未来三天：";
  for (size_t index = 0; index < forecastCount; ++index) {
    if (index > 0) aiContext += "，";
    aiContext += dayLabels[index] + " " + conditions[index] + " " +
                 String(lowC[index]) + "~" + String(highC[index]) + "℃";
  }
  aiContext = utf8Clip(aiContext, sizeof(result.aiContext) - 1);
  strlcpy(result.aiContext, aiContext.c_str(), sizeof(result.aiContext));
  Serial.printf("[天气] 高德 %s %.1f C，%s；已更新 %u 天预报。\n", city.c_str(),
                temperature, condition.c_str(), static_cast<unsigned>(forecastCount));
  return true;
}

void weatherWorkerTask(void *) {
  WeatherRequest request{};
  for (;;) {
    if (xQueueReceive(weatherRequestQueue, &request, portMAX_DELAY) != pdTRUE) continue;
    const String city(request.city);
    WeatherResult result{};
    result.locateByIp = request.locateByIp;
    strlcpy(result.query, request.city, sizeof(result.query));
    strlcpy(result.city, request.city, sizeof(result.city));
    strlcpy(result.adcode, request.adcode, sizeof(result.adcode));
    result.latitude = request.latitude;
    result.longitude = request.longitude;
    if (request.resolveLocation) {
      String resolvedCity(result.city);
      String resolvedAdcode(result.adcode);
      result.locationResolved = resolveWeatherLocation(
          city, String(request.apiKey), request.locateByIp, resolvedCity,
          resolvedAdcode, result.latitude, result.longitude);
      if (!result.locationResolved) {
        if (weatherResultQueue) xQueueOverwrite(weatherResultQueue, &result);
        continue;
      }
      strlcpy(result.city, resolvedCity.c_str(), sizeof(result.city));
      strlcpy(result.adcode, resolvedAdcode.c_str(), sizeof(result.adcode));
    }
    result.success = refreshWeather(String(result.city), String(result.adcode),
                                    String(request.apiKey), result);
    if (weatherResultQueue) xQueueOverwrite(weatherResultQueue, &result);
  }
}

bool initializeWeatherWorker() {
  if (weatherWorkerHandle) return true;
  weatherRequestQueue = xQueueCreate(1, sizeof(WeatherRequest));
  weatherResultQueue = xQueueCreate(1, sizeof(WeatherResult));
  if (!weatherRequestQueue || !weatherResultQueue) {
    if (weatherRequestQueue) vQueueDelete(weatherRequestQueue);
    if (weatherResultQueue) vQueueDelete(weatherResultQueue);
    weatherRequestQueue = nullptr;
    weatherResultQueue = nullptr;
    Serial.printf("[天气] 创建队列失败，free=%u max=%u。\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    return false;
  }
  constexpr BaseType_t WEATHER_WORKER_CORE = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
  if (xTaskCreatePinnedToCore(weatherWorkerTask, "weather-worker", 6144, nullptr, 1,
                              &weatherWorkerHandle, WEATHER_WORKER_CORE) != pdPASS) {
    vQueueDelete(weatherRequestQueue);
    vQueueDelete(weatherResultQueue);
    weatherRequestQueue = nullptr;
    weatherResultQueue = nullptr;
    Serial.printf("[天气] 创建常驻任务失败，free=%u max=%u。\n", ESP.getFreeHeap(),
                  ESP.getMaxAllocHeap());
    return false;
  }
  Serial.printf("[天气] 常驻任务已创建，free=%u max=%u。\n", ESP.getFreeHeap(),
                ESP.getMaxAllocHeap());
  return true;
}

bool startWeatherRefresh() {
  if (weatherRefreshRunning) return true;
  if (!weatherWorkerHandle && !initializeWeatherWorker()) {
    RobotDisplay::setWeatherStatus(settings.weatherCity, "任务失败 重试");
    return false;
  }
  WeatherRequest request{};
  strlcpy(request.city, settings.weatherCity.c_str(), sizeof(request.city));
  strlcpy(request.adcode, settings.weatherAdcode.c_str(), sizeof(request.adcode));
  strlcpy(request.apiKey, settings.amapKey.c_str(), sizeof(request.apiKey));
  request.latitude = settings.weatherLatitude;
  request.longitude = settings.weatherLongitude;
  request.resolveLocation = weatherLocationNeedsResolution;
  request.locateByIp = settings.weatherLocationMode == "ip";
  if (xQueueSend(weatherRequestQueue, &request, 0) != pdTRUE) {
    Serial.println(F("[天气] 工作队列忙，15 秒后重试。"));
    RobotDisplay::setWeatherStatus(settings.weatherCity, "队列忙 重试");
    return false;
  }
  weatherRefreshRunning = true;
  return true;
}

bool applyConfig(const String &json) {
  JsonDocument doc;
  if (deserializeJson(doc, json) || !doc.is<JsonObject>()) return false;
  const Settings previous = settings;
  const bool weatherAdcodeProvided = doc["weather_adcode"].is<const char *>();
  if (doc["ssid"].is<const char *>()) settings.wifiSsid = doc["ssid"].as<String>();
  if (settings.wifiSsid != previous.wifiSsid && !doc["wifi_password"].is<const char *>()) {
    const auto *saved = WifiProfiles::find(settings.wifiSsid);
    settings.wifiPassword = saved ? saved->password : "";
  }
  if (doc["wifi_password"].is<const char *>()) settings.wifiPassword = doc["wifi_password"].as<String>();
  if (doc["server_url"].is<const char *>()) settings.serverUrl = normalizedUrl(doc["server_url"].as<String>());
  if (doc["device_id"].is<const char *>()) settings.deviceId = doc["device_id"].as<String>();
  if (doc["device_token"].is<const char *>()) settings.deviceToken = doc["device_token"].as<String>();
  if (!doc["admin_pin"].isNull()) {
    String pinValue = doc["admin_pin"].is<const char *>()
                          ? doc["admin_pin"].as<String>()
                          : String(doc["admin_pin"].as<int>());
    bool validPin = pinValue.length() == 6;
    for (size_t index = 0; validPin && index < pinValue.length(); ++index) {
      validPin = isDigit(pinValue[index]);
    }
    if (!validPin) {
      settings = previous;
      return false;
    }
    settings.adminPin = static_cast<uint32_t>(pinValue.toInt());
  }
  if (doc["volume_percent"].is<int>()) {
    settings.volumePercent = constrain(doc["volume_percent"].as<int>(), 0, 100);
    settings.volumePotentiometer = false;
  }
  if (doc["volume_mode"].is<const char *>()) {
    String mode = doc["volume_mode"].as<String>();
    mode.toLowerCase();
    if (mode != "fixed" && mode != "pot") {
      settings = previous;
      return false;
    }
    settings.volumePotentiometer = mode == "pot";
  }
  if (settings.volumePotentiometer && (RobotDisplay::potentiometerBrightnessEnabled() ||
                                        RobotDisplay::BATTERY_MONITOR_ENABLED)) {
    settings = previous;
    return false;
  }
  if (doc["ota_auto_update"].is<bool>()) {
    settings.otaAutoUpdate = doc["ota_auto_update"].as<bool>();
  }
  if (doc["offline_wake_enabled"].is<bool>()) {
    settings.offlineWakeEnabled = doc["offline_wake_enabled"].as<bool>();
  }
  if (doc["offline_wake_model"].is<const char *>()) {
    settings.offlineWakeModel = doc["offline_wake_model"].as<String>();
  }
  if (doc["display_pages"].is<int>()) {
    settings.displayPages = doc["display_pages"].as<int>() & RobotDisplay::PAGE_ALL;
    if (settings.displayPages == 0) {
      settings = previous;
      return false;
    }
  } else if (doc["display_carousel"].is<bool>()) {
    settings.displayPages = doc["display_carousel"].as<bool>() ? 0x03 : 0x01;
  }
  if (doc["amap_key"].is<const char *>()) settings.amapKey = doc["amap_key"].as<String>();
  if (doc["weather_city"].is<const char *>()) {
    settings.weatherCity = doc["weather_city"].as<String>();
  }
  if (weatherAdcodeProvided) settings.weatherAdcode = doc["weather_adcode"].as<String>();
  if (doc["weather_location_mode"].is<const char *>()) {
    settings.weatherLocationMode = doc["weather_location_mode"].as<String>();
  }
  if (settings.weatherCity != previous.weatherCity && !weatherAdcodeProvided) {
    settings.weatherAdcode = "";
    settings.weatherLocationMode = "manual";
  }
  if (doc["weather_latitude"].is<float>() || doc["weather_latitude"].is<int>()) {
    settings.weatherLatitude = doc["weather_latitude"].as<float>();
  }
  if (doc["weather_longitude"].is<float>() || doc["weather_longitude"].is<int>()) {
    settings.weatherLongitude = doc["weather_longitude"].as<float>();
  }
  const bool voiceChanged = doc["chinese_speaker_id"].is<int>() ||
                            doc["english_speaker_id"].is<int>() ||
                            doc["speech_speed"].is<float>() || doc["speech_speed"].is<int>();
  if (doc["chinese_speaker_id"].is<int>()) settings.chineseSpeakerId = constrain(doc["chinese_speaker_id"].as<int>(), 0, 4);
  if (doc["english_speaker_id"].is<int>()) settings.englishSpeakerId = constrain(doc["english_speaker_id"].as<int>(), 0, 903);
  if (doc["speech_speed"].is<float>() || doc["speech_speed"].is<int>()) settings.speechSpeed = constrain(doc["speech_speed"].as<float>(), 0.5f, 2.0f);
  if (settings.wifiSsid.length() > 32 || settings.wifiPassword.length() > 63 ||
      (!settings.wifiPassword.isEmpty() && settings.wifiPassword.length() < 8) ||
      settings.serverUrl.length() > 180 ||
      !(settings.serverUrl.startsWith("http://") || settings.serverUrl.startsWith("https://")) ||
      settings.deviceId.length() < 3 || settings.deviceId.length() > 64 || settings.deviceToken.length() > 256 ||
      settings.adminPin < 100000 || settings.adminPin > 999999 ||
      !OfflineWake::isSupportedModel(settings.offlineWakeModel) ||
      settings.amapKey.length() > 64 ||
      settings.weatherCity.isEmpty() || settings.weatherCity.length() > 64 ||
      (!settings.weatherAdcode.isEmpty() && !validAdcode(settings.weatherAdcode)) ||
      (settings.weatherLocationMode != "manual" && settings.weatherLocationMode != "ip") ||
      settings.weatherLatitude < -90.0f || settings.weatherLatitude > 90.0f ||
      settings.weatherLongitude < -180.0f || settings.weatherLongitude > 180.0f) {
    settings = previous;
    return false;
  }
  conversation.cancel();
  VoiceInput::setPlaybackVolume(settings.volumePercent);
  VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
  RobotDisplay::setPageMask(settings.displayPages);
  activeServerUrl = "";
  closeCloudConnection();
  const bool wifiEdited = doc["ssid"].is<const char *>() || doc["wifi_password"].is<const char *>();
  if (!saveSettings() || (wifiEdited && !settings.wifiSsid.isEmpty() &&
                         !WifiProfiles::save(settings.wifiSsid, settings.wifiPassword))) {
    settings = previous;
    VoiceInput::setPlaybackVolume(settings.volumePercent);
    VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
    RobotDisplay::setPageMask(settings.displayPages);
    OfflineWake::configure(settings.offlineWakeEnabled,
                           settings.offlineWakeModel);
    saveSettings();
    return false;
  }
  traceRuntime("config_saved", "设备配置已写入 NVS");
  const bool weatherSelectionChanged =
      settings.weatherCity != previous.weatherCity ||
      settings.weatherAdcode != previous.weatherAdcode ||
      settings.weatherLocationMode != previous.weatherLocationMode ||
      settings.amapKey != previous.amapKey;
  if (weatherSelectionChanged) {
    weatherAiContext = "";
    weatherAiUpdatedAtMs = 0;
  }
  weatherLocationNeedsResolution = settings.weatherLocationMode == "ip" ||
                                   !validAdcode(settings.weatherAdcode);
  nextWeatherRefreshAtMs = millis() + 1000;
  weatherRefreshRequested = true;
  if (!OfflineWake::configure(settings.offlineWakeEnabled,
                              settings.offlineWakeModel)) {
    traceRuntime("wake_config_error", OfflineWake::lastError(), true);
  } else {
    traceRuntime(settings.offlineWakeEnabled ? "wake_enabled" : "wake_disabled",
                 settings.offlineWakeEnabled
                     ? "离线唤醒词已切换为“" +
                           String(OfflineWake::wakeWord()) + "”"
                     : "离线唤醒已关闭");
  }
  pendingWifiReconnect = doc["connect"] | true;
  if (pendingWifiReconnect && wifiEdited) preferConfiguredWifi = true;
  pendingVoiceSync = pendingVoiceSync || voiceChanged;
  if (settings.adminPin != previous.adminPin) {
    WebAdmin::invalidateAll();
    traceRuntime("pin_changed", "设备连接码已更新，请重新登录后台");
  }
  return true;
}

bool validWebPin(const String &value) {
  if (webPinLockedUntilMs != 0 &&
      static_cast<int32_t>(millis() - webPinLockedUntilMs) < 0) return false;
  webPinLockedUntilMs = 0;
  bool numeric = value.length() == 6;
  for (size_t index = 0; numeric && index < value.length(); ++index) {
    numeric = isDigit(value[index]);
  }
  if (numeric && value.toInt() == static_cast<int>(settings.adminPin)) {
    webPinFailures = 0;
    return true;
  }
  if (++webPinFailures >= 5) {
    webPinFailures = 0;
    webPinLockedUntilMs = millis() + 60000;
  }
  return false;
}

bool amapSearchLocations(const String &query, String &output, String &errorMessage) {
  JsonDocument source;
  const String path = "/v3/assistant/inputtips?output=JSON&datatype=all&keywords=" +
                      urlEncodeComponent(query);
  if (!amapApiGet(path, settings.amapKey, source, errorMessage)) return false;

  JsonDocument response;
  response["ok"] = true;
  JsonArray items = response["items"].to<JsonArray>();
  size_t accepted = 0;
  for (JsonObjectConst tip : source["tips"].as<JsonArrayConst>()) {
    if (accepted >= 8) break;
    const String adcode = jsonScalarText(tip["adcode"]);
    const String name = jsonScalarText(tip["name"]);
    if (!validAdcode(adcode) || name.isEmpty()) continue;
    JsonObject item = items.add<JsonObject>();
    item["name"] = name;
    item["district"] = jsonScalarText(tip["district"]);
    item["adcode"] = adcode;
    float latitude = 0.0f;
    float longitude = 0.0f;
    if (parseAmapLocation(jsonScalarText(tip["location"]), latitude, longitude)) {
      item["latitude"] = latitude;
      item["longitude"] = longitude;
    }
    ++accepted;
  }
  if (accepted == 0) {
    errorMessage = "未找到可用于天气的地址";
    return false;
  }
  serializeJson(response, output);
  return true;
}

bool amapIpLocation(String &output, String &errorMessage) {
  String city = "IP定位";
  String adcode;
  float latitude = 0.0f;
  float longitude = 0.0f;
  if (!resolveWeatherLocation(city, settings.amapKey, true, city, adcode,
                              latitude, longitude)) {
    errorMessage = "当前公网IP无法定位到国内城市";
    return false;
  }
  JsonDocument response;
  response["ok"] = true;
  response["item"]["name"] = city;
  response["item"]["district"] = "IP自动定位";
  response["item"]["adcode"] = adcode;
  response["item"]["latitude"] = latitude;
  response["item"]["longitude"] = longitude;
  serializeJson(response, output);
  return true;
}

String alarmWeekdaysText(uint8_t mask) {
  if (mask == 0x7F) return "每天";
  if (mask == 0x3E) return "工作日";
  if (mask == 0x41) return "周末";
  static const char *names[] = {"日", "一", "二", "三", "四", "五", "六"};
  String text = "周";
  for (uint8_t day = 0; day < 7; ++day) if (mask & (1U << day)) text += names[day];
  return text;
}

String alarmEntryText(const AlarmManager::Entry &entry) {
  if (entry.type == AlarmManager::Type::Recurring) {
    char clockText[8];
    snprintf(clockText, sizeof(clockText), "%02u:%02u", entry.hour, entry.minute);
    return alarmWeekdaysText(entry.weekdays) + " " + clockText + " · " + entry.label;
  }
  const time_t when = static_cast<time_t>(entry.triggerAt);
  struct tm local {};
  char timeText[24] = "时间待同步";
  if (when >= 1700000000 && localtime_r(&when, &local)) {
    strftime(timeText, sizeof(timeText), "%m-%d %H:%M:%S", &local);
  }
  return String(entry.type == AlarmManager::Type::Timer ? "计时器 " : "单次 ") +
         timeText + " · " + entry.label;
}


void appendWifiPage(WebAdmin::PageStream &page) {
  page += FPSTR(WIFI_SETTINGS_PAGE);
}

void registerAdminRoutes() {
  phoneWeb.on("/setup", HTTP_GET, []() {
    phoneWeb.sendHeader("Cache-Control", "no-store");
    if (WebAdmin::authenticated(phoneWeb)) {
      phoneWeb.sendHeader("Location", "/#network"); phoneWeb.send(303); return;
    }
    phoneWeb.send_P(200, "text/html; charset=utf-8", DisplayWeb::SETUP_PAGE);
  });
  phoneWeb.on("/login", HTTP_GET, []() {
    if (WebAdmin::authenticated(phoneWeb)) {
      phoneWeb.sendHeader("Location", "/");
      phoneWeb.send(303);
      return;
    }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8", F(
        "<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>设备后台登录</title><style>body{font-family:system-ui;max-width:360px;"
        "margin:12vh auto;padding:20px}input,button{box-sizing:border-box;width:100%;padding:12px;margin:12px 0}</style>"
        "<h1>设备后台登录</h1><form method='post' action='/login'>"
        "<label>设备连接码</label><input type='password' name='pin' inputmode='numeric' minlength='6' maxlength='6' "
        "pattern='[0-9]{6}' autocomplete='current-password' required autofocus>"
        "<button type='submit'>登录</button></form><p>首次默认连接码：123456。登录后操作无需重复输入。</p></html>"));
  });
  phoneWeb.on("/login", HTTP_POST, []() {
    if (!validWebPin(phoneWeb.arg("pin"))) {
      phoneWeb.send(403, "text/html; charset=utf-8", F(
          "<!doctype html><meta charset='utf-8'><p>连接码错误或暂时锁定。连续输错5次后需等待60秒。</p><a href='/setup'>返回登录</a>"));
      return;
    }
    WebAdmin::login(phoneWeb);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.sendHeader("Location", phoneWeb.arg("next") == "setup" ? "/#network" : "/");
    phoneWeb.send(303);
  });
  phoneWeb.on("/logout", HTTP_POST, []() {
    WebAdmin::logout(phoneWeb);
    phoneWeb.sendHeader("Location", "/login");
    phoneWeb.send(303);
  });
  phoneWeb.on("/wifi/saved", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    JsonDocument doc;
    doc["connected"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "";
    JsonArray items = doc["networks"].to<JsonArray>();
    for (size_t i = 0; i < WifiProfiles::count(); ++i) items.add(WifiProfiles::get(i)->ssid);
    String output;
    serializeJson(doc, output);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", output);
  });
  phoneWeb.on("/wifi/scan", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) { phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在连接网络或处理语音，请稍后扫描。"); return; }
    if (phoneWeb.hasArg("start")) WifiScan::start();
    WifiScan::poll();
    JsonDocument doc;
    doc["scanning"] = WifiScan::running();
    doc["failed"] = WifiScan::failed();
    JsonArray items = doc["networks"].to<JsonArray>();
    for (size_t i = 0; i < WifiScan::count(); ++i) {
      const auto &network = WifiScan::get(i);
      JsonObject item = items.add<JsonObject>();
      item["ssid"] = network.ssid;
      item["rssi"] = network.rssi;
      item["open"] = network.open;
      item["saved"] = WifiProfiles::find(network.ssid) != nullptr;
    }
    String output;
    serializeJson(doc, output);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", output);
  });
  phoneWeb.on("/wifi/save", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) { phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在连接网络或处理语音，请稍后保存网络。"); return; }
    const String ssid = phoneWeb.arg("ssid");
    String password = phoneWeb.arg("wifi_password");
    const bool open = phoneWeb.arg("open") == "1";
    const auto *saved = WifiProfiles::find(ssid);
    if (open) password = "";
    else if (password.isEmpty() && saved) password = saved->password;
    else if (password.isEmpty()) {
      phoneWeb.send(422, "text/html; charset=utf-8", autoReturnPage("网络未保存", "新网络需要密码；无密码网络请勾选对应选项。", 4));
      return;
    }
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["wifi_password"] = password;
    doc["connect"] = true;
    String payload;
    serializeJson(doc, payload);
    if (!applyConfig(payload)) {
      phoneWeb.send(422, "text/html; charset=utf-8", autoReturnPage("网络未保存", "请检查名称/密码长度，或稍后重试存储。", 4));
      return;
    }
    phoneWeb.send(200, "text/html; charset=utf-8", autoReturnPage("Wi-Fi 已保存", "正在连接。成功后请使用屏幕最上方显示的 IP 进入后台；配置热点将自动关闭。", 15));
  });
  phoneWeb.on("/wifi/forget", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) { phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在忙，请稍后删除网络。"); return; }
    const String ssid = phoneWeb.arg("ssid");
    if (!WifiProfiles::remove(ssid)) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "网络不存在或删除失败。");
      return;
    }
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid) {
      WiFi.setAutoReconnect(false);
    }
    if (settings.wifiSsid == ssid) {
      settings.wifiSsid = "";
      settings.wifiPassword = "";
      if (!saveSettings()) {
        phoneWeb.send(500, "text/plain; charset=utf-8", "已从网络列表删除，但旧版配置清理失败，请重新保存设置。");
        return;
      }
    }
    phoneWeb.send(200, "text/plain; charset=utf-8", "已忘记；当前连接暂时保留，以后不再自动选择此网络。");
  });
}

void serviceFocusTimer() {
  String announcement;
  if (FocusTimer::poll(announcement)) pendingFocusAnnouncement = announcement;
  if (FocusTimer::focusing() || AlarmManager::ringing() || pendingOtaCheck || pendingOtaInstall || pendingDisplayRestartAtMs)
    RobotInteraction::stop();
  else RobotInteraction::poll();
  if (!FocusTimer::active()) pendingFocusAnnouncement = "";
  if (millis() - lastFocusDisplayMs >= 1000U || lastFocusDisplayMs == 0) {
    RobotDisplay::setFocusStatus(FocusTimer::active(), FocusTimer::displaySummary());
    lastFocusDisplayMs = millis();
  }
}

String profileStatusJson(const String &message = "") {
  JsonDocument doc;
  PersonalProfile::appendSnapshot(doc["profile"].to<JsonObject>());
  doc["ready"] = PersonalProfile::ready();
  doc["synced"] = profileSyncConfirmed && profileSyncedRevision == PersonalProfile::revision();
  doc["sync_message"] = profileSyncMessage;
  doc["storage"] = VoiceInput::recordingStorageName();
  doc["message"] = message.isEmpty() ? String(PersonalProfile::lastError()) : message;
  String out; serializeJson(doc, out); return out;
}

bool syncPersonalProfile(bool restore = false) {
  if ((!restore && !PersonalProfile::ready()) || WiFi.status() != WL_CONNECTED || settings.deviceToken.isEmpty()) {
    profileSyncMessage = "本机记录保留，等待存储或网络就绪"; return false;
  }
  cloudHttp.setConnectTimeout(3000); cloudHttp.setTimeout(4000);
  if (!beginCloudRequest(requestBaseUrl() + "/voice-api/v1/personal-profile")) return false;
  addAuthHeaders(cloudHttp);
  int code;
  if (restore) code = cloudHttp.GET();
  else {
    cloudHttp.addHeader("Content-Type", "application/json");
    code = cloudHttp.sendRequest("PUT", PersonalProfile::toJson());
  }
  JsonDocument reply;
  const bool decoded = code == HTTP_CODE_OK && cloudHttp.getSize() <= 16384 &&
      !deserializeJson(reply, cloudHttp.getStream());
  endCloudRequest();
  if (decoded && reply["schema"].as<int>() == 1 && reply["revision"].is<uint32_t>()) {
    if (restore) {
      if (!PersonalProfile::restore(reply.as<JsonVariantConst>())) {
        profileSyncMessage = PersonalProfile::lastError(); return false;
      }
      profileSyncConfirmed = false; profileSyncRequested = true;
      profileSyncMessage = "已恢复到设备，等待同步新版本"; return true;
    }
    if (reply["revision"].as<uint32_t>() == PersonalProfile::revision()) {
      profileSyncedRevision = PersonalProfile::revision(); profileSyncConfirmed = true;
      profileSyncRequested = false; profileSyncMessage = "云端已同步"; return true;
    }
  }
  profileSyncConfirmed = false;
  profileSyncMessage = code == 409 ? "版本冲突，请先查看或恢复云端备份" :
      "云端同步未成功，HTTP " + String(code) + "；本机记录已保留";
  return false;
}

String microphoneStatusJson(const String &message = "", bool ok = true) {
  JsonDocument doc;
  doc["ok"] = ok;
  doc["mask"] = VoiceInput::microphoneMask();
  doc["preferred"] = VoiceInput::preferredMicrophone();
  doc["selected"] = VoiceInput::selectedMicrophone();
  doc["detected_mask"] = VoiceInput::microphoneProbeMask();
  doc["message"] = message;
  doc["diagnostics"] = VoiceInput::microphoneDiagnostics();
  String out; serializeJson(doc, out); return out;
}

void registerMicrophoneRoutes() {
  phoneWeb.on("/microphones", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", microphoneStatusJson());
  });
  phoneWeb.on("/microphones", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || conversation.active() || pendingWebRecording || AlarmManager::ringing() ||
        CustomWake::trainingPending() || CustomWake::trainingActive() || MultiMicConfig::busy()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请在录音、对话、闹钟或麦克风检测结束后设置。"); return;
    }
    JsonDocument doc;
    const String body = phoneWeb.arg("plain");
    if (body.length() > 256 || deserializeJson(doc, body) || !doc["action"].is<const char *>()) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "麦克风请求无效。"); return;
    }
    const String action = doc["action"].as<String>();
    uint8_t mask = VoiceInput::microphoneMask(), preferred = VoiceInput::preferredMicrophone();
    if (action == "config") {
      if (!doc["count"].is<uint8_t>() || doc["count"].as<uint8_t>() < 1 ||
          doc["count"].as<uint8_t>() > 3 || !doc["preferred"].is<uint8_t>() ||
          doc["preferred"].as<uint8_t>() > doc["count"].as<uint8_t>()) {
        phoneWeb.send(422, "text/plain; charset=utf-8", "麦克风数量或固定通道无效。"); return;
      }
      mask = (1U << doc["count"].as<uint8_t>()) - 1U;
      preferred = doc["preferred"].as<uint8_t>();
    } else if (action != "probe") {
      phoneWeb.send(422, "text/plain; charset=utf-8", "麦克风操作无效。"); return;
    }
    FullMusic::Foreground musicGuard;
    if (!musicGuard.ready()) { phoneWeb.send(503, "text/plain", "音乐连接尚未关闭，请稍后重试"); return; }
    OfflineWake::suspend();
    busy = true;
    String message;
    const bool ok = action == "config" ? MultiMicConfig::apply(mask, preferred, message) :
        MultiMicConfig::executeCommand("test", message);
    busy = false;
    OfflineWake::resume();
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(ok ? 200 : 422, "application/json; charset=utf-8", microphoneStatusJson(message, ok));
  });
}

void registerCompanionRoutes() {
  phoneWeb.on("/interaction", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", RobotInteraction::toJson());
  });
  phoneWeb.on("/interaction", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || conversation.active() || pendingWebRecording || CustomWake::trainingPending()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话或声音录入。"); return;
    }
    JsonDocument doc;
    const String body = phoneWeb.arg("plain");
    if (body.length() > 600 || deserializeJson(doc, body) || !doc["action"].is<const char *>()) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "互动动作请求无效。"); return;
    }
    const String action = doc["action"].as<String>();
    bool ok = false;
    if (action == "config" && doc["enabled"].is<bool>() && doc["channel"].is<uint8_t>() &&
        doc["low_degrees"].is<uint8_t>() && doc["high_degrees"].is<uint8_t>()) {
      RobotInteraction::Config config;
      config.enabled = doc["enabled"].as<bool>(); config.channel = doc["channel"].as<uint8_t>();
      config.lowDegrees = doc["low_degrees"].as<uint8_t>(); config.highDegrees = doc["high_degrees"].as<uint8_t>();
      ok = RobotInteraction::configure(config);
    } else if (action == "preview") {
      if (AlarmManager::ringing() || pendingOtaCheck || pendingOtaInstall || pendingDisplayRestartAtMs ||
          RobotControls::talkPressed() || RobotControls::abortRequested()) {
        phoneWeb.send(409, "text/plain; charset=utf-8", "设备当前有优先任务，请空闲后再预览。"); return;
      }
      if (FocusTimer::focusing()) {
        phoneWeb.send(409, "text/plain; charset=utf-8", "专注期间暂停互动动作。"); return;
      }
      ok = RobotInteraction::onReply(0);
    } else { phoneWeb.send(422, "text/plain; charset=utf-8", "互动动作操作或参数无效。"); return; }
    JsonDocument reply; deserializeJson(reply, RobotInteraction::toJson());
    reply["ok"] = ok;
    if (!ok) reply["message"] = strlen(RobotInteraction::lastError()) ? RobotInteraction::lastError() : "动作未启用或当前不可执行。";
    String out; serializeJson(reply, out);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(ok ? 200 : 422, "application/json; charset=utf-8", out);
  });
  phoneWeb.on("/profile", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", profileStatusJson());
  });
  phoneWeb.on("/profile", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || conversation.active() || pendingWebRecording || CustomWake::trainingPending()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束对话或声音录入。"); return;
    }
    JsonDocument doc;
    const String body = phoneWeb.arg("plain");
    if (body.length() > 1500 || deserializeJson(doc, body) || !doc["action"].is<const char *>()) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "个人记忆请求无效。"); return;
    }
    const String action = doc["action"].as<String>();
    bool ok = false;
    if (action == "policy" && doc["enabled"].is<bool>() && doc["auto_collect"].is<bool>() && doc["backup_enabled"].is<bool>())
      ok = PersonalProfile::setPolicy(doc["enabled"].as<bool>(), doc["auto_collect"].as<bool>(), doc["backup_enabled"].as<bool>());
    else if (action == "add" && doc["category"].is<const char *>() && doc["value"].is<const char *>())
      ok = PersonalProfile::add(doc["category"].as<String>(), doc["value"].as<String>());
    else if (action == "edit" && doc["id"].is<uint32_t>() && doc["value"].is<const char *>())
      ok = PersonalProfile::edit(doc["id"].as<uint32_t>(), doc["value"].as<String>());
    else if (action == "remove" && doc["id"].is<uint32_t>()) ok = PersonalProfile::remove(doc["id"].as<uint32_t>());
    else if (action == "clear") ok = PersonalProfile::clear();
    else if (action == "sync" || action == "restore") {
      ok = syncPersonalProfile(action == "restore");
      phoneWeb.sendHeader("Cache-Control", "no-store");
      phoneWeb.send(ok ? 200 : 502, "application/json; charset=utf-8", profileStatusJson(profileSyncMessage)); return;
    } else { phoneWeb.send(422, "text/plain; charset=utf-8", "个人记忆操作或参数无效。"); return; }
    if (ok) { profileSyncRequested = true; profileSyncConfirmed = false; nextProfileSyncMs = 0; }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(ok ? 200 : 422, "application/json; charset=utf-8",
        profileStatusJson(ok ? "已保存到设备，云端同步状态见下方。" : String(PersonalProfile::lastError())));
  });
  phoneWeb.on("/focus", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    serviceFocusTimer();
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", FocusTimer::toJson());
  });
  phoneWeb.on("/focus", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || conversation.active() || pendingWebRecording || CustomWake::trainingPending()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话或声音录入。"); return;
    }
    JsonDocument doc;
    const String body = phoneWeb.arg("plain");
    if (body.length() > 1000 || deserializeJson(doc, body) || !doc["action"].is<const char *>()) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "番茄钟请求无效。"); return;
    }
    FocusTimer::Config config = FocusTimer::getConfig();
    const char *names[] = {"focus_minutes", "short_break_minutes", "long_break_minutes", "rounds_before_long_break"};
    bool hasConfig = false;
    for (const char *name : names) if (!doc[name].isNull()) {
      hasConfig = true;
      if (!doc[name].is<uint16_t>() || doc[name].as<uint16_t>() == 0 || doc[name].as<uint16_t>() > (String(name) == "rounds_before_long_break" ? 12 : 120)) {
        phoneWeb.send(422, "text/plain; charset=utf-8", "番茄钟时长或轮数无效。"); return;
      }
    }
    if (!doc["focus_minutes"].isNull()) config.focusMinutes = doc["focus_minutes"].as<uint16_t>();
    if (!doc["short_break_minutes"].isNull()) config.shortBreakMinutes = doc["short_break_minutes"].as<uint16_t>();
    if (!doc["long_break_minutes"].isNull()) config.longBreakMinutes = doc["long_break_minutes"].as<uint16_t>();
    if (!doc["rounds_before_long_break"].isNull()) config.roundsBeforeLongBreak = doc["rounds_before_long_break"].as<uint8_t>();
    const String action = doc["action"].as<String>();
    const auto result = FocusTimer::control(action, hasConfig ? &config : nullptr);
    serviceFocusTimer();
    if (action == "stop" && result.changed) pendingFocusAnnouncement = "";
    JsonDocument reply; deserializeJson(reply, FocusTimer::toJson());
    reply["ok"] = result.changed || action == "state";
    reply["message"] = result.message;
    String out; serializeJson(reply, out);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(result.changed || action == "state" ? 200 : 422, "application/json; charset=utf-8", out);
  });
  phoneWeb.on("/wake/custom", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", CustomWake::toJson());
  });
  phoneWeb.on("/wake/custom", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    JsonDocument doc;
    const String body = phoneWeb.arg("plain");
    if (body.length() > 1000 || deserializeJson(doc, body) || !doc["action"].is<const char *>()) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "唤醒词请求无效。"); return;
    }
    const String action = doc["action"].as<String>();
    if (action == "cancel") { CustomWake::cancelTraining(); phoneWeb.send(200, "application/json", CustomWake::toJson()); return; }
    if (busy || conversation.active() || pendingWebRecording || CustomWake::trainingPending() || CustomWake::trainingActive()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话或声音录入。"); return;
    }
    bool ok = false;
    if (action == "label" && doc["label"].is<const char *>()) ok = CustomWake::setLabel(doc["label"].as<String>());
    else if (action == "enable" && doc["enabled"].is<bool>()) ok = CustomWake::enable(doc["enabled"].as<bool>());
    else if (action == "train") ok = CustomWake::queueTraining();
    else if (action == "reset") ok = CustomWake::reset();
    else { phoneWeb.send(422, "text/plain; charset=utf-8", "唤醒词操作或参数无效。"); return; }
    if (ok && action != "train") OfflineWake::resume();
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(ok ? (action == "train" ? 202 : 200) : 422, "application/json; charset=utf-8", CustomWake::toJson());
  });
}

void registerMusicRoutes() {
  phoneWeb.on("/music/status", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store"); phoneWeb.send(200, "application/json; charset=utf-8", FullMusic::statusJson());
  });
  phoneWeb.on("/music/control", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    JsonDocument doc;
    if (phoneWeb.arg("plain").length() > 1024 || deserializeJson(doc, phoneWeb.arg("plain"))) { phoneWeb.send(422); return; }
    const String action = doc["action"] | "", value = doc["value"] | "";
    bool ok = true;
    if (action == "play") ok = FullMusic::play(value);
    else if (action == "search") ok = FullMusic::search(value);
    else if (action == "pause") FullMusic::pause();
    else if (action == "resume") FullMusic::resume();
    else if (action == "stop") FullMusic::stop();
    else if (action == "next") FullMusic::next();
    else if (action == "previous") FullMusic::next(true);
    else if (action == "playlist") FullMusic::selectPlaylist(value);
    else if (action == "volume") { const int volume = value.toInt(); if (volume < 0 || volume > 100) ok = false;
      else { settings.volumePercent = volume; settings.volumePotentiometer = false;
        VoiceInput::usePotentiometerVolume(false); VoiceInput::setPlaybackVolume(volume); ok = saveSettings(); } }
    else ok = false;
    phoneWeb.send(ok ? 200 : 422, "application/json", ok ? FullMusic::statusJson() : "{\"error\":\"音乐操作无效\"}");
  });
  auto proxy = [](bool ticket) {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingWebRecording || conversation.active() || AlarmManager::ringing() || CustomWake::trainingActive()) { phoneWeb.send(409, "text/plain", "助手处理任务中，请稍后重试"); return; }
    JsonDocument doc;
    const String source = phoneWeb.arg("plain");
    if (source.length() > 4096 || deserializeJson(doc, source)) { phoneWeb.send(422); return; }
    String path, method = "POST", body;
    if (ticket) { path = "/music/upload-tickets"; serializeJson(doc, body); }
    else {
      path = doc["path"] | ""; method = doc["method"] | "GET";
      if (!(path == "/music" || path.startsWith("/music?") || path.startsWith("/music/")) || path.indexOf("..") >= 0 ||
          path.indexOf(":") >= 0 || path.indexOf("\\") >= 0 || path.indexOf("/pcm") >= 0 || path.indexOf("browser-upload") >= 0 ||
          (method != "GET" && method != "POST" && method != "DELETE")) { phoneWeb.send(422); return; }
      if (!doc["data"].isNull()) serializeJson(doc["data"], body);
    }
    String response; int code; FullMusic::request(path, method, body, response, code);
    phoneWeb.send(code > 0 ? code : 502, "application/json; charset=utf-8", response);
  };
  phoneWeb.on("/music/cloud", HTTP_POST, [proxy]() { proxy(false); });
  phoneWeb.on("/music/upload-ticket", HTTP_POST, [proxy]() { proxy(true); });
  phoneWeb.on("/alarm/music/preview", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || conversation.active() || AlarmManager::ringing() || pendingWebRecording || alarmPreview) { phoneWeb.send(409); return; }
    if (!FullMusic::hold()) { phoneWeb.send(503); return; }
    busy = true; const bool ok = loadAlarmMusic(); busy = false;
    alarmPreview = ok; alarmPreviewStarted = millis();
    if (!ok) FullMusic::release();
    phoneWeb.send(ok ? 200 : 502, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"铃声试听失败\"}");
  });
  phoneWeb.on("/alarm/music/preview/stop", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (alarmPreview) { alarmPreview = false; MusicPlayer::stop(); FullMusic::release(); }
    phoneWeb.send(200, "application/json", "{\"ok\":true}");
  });
}

void startPhoneWeb() {
  if (webStarted) return;
  WebAdmin::begin(phoneWeb);
  registerAdminRoutes();
  RobotPhoto::registerRoutes(phoneWeb, []() {
    return !busy && pendingDisplayRestartAtMs == 0 && !pendingOtaCheck && !pendingOtaInstall;
  }, []() {
    RobotDisplay::setPhotoMode(true);
    returnToHomeDisplay();
  });
  phoneWeb.on("/", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb, true)) return;
    WebAdmin::PageStream page(phoneWeb);
    page += FPSTR(ADMIN_PAGE_HEAD);
    page += F("<section class='panel' data-panel='home'><h2>今天的机器人</h2><p class='intro'>常用操作集中在这里，更多配置从左侧进入。</p>"
              "<div class='quick-grid'><a href='#display'>屏幕与相册<span>上传多张图片、切换照片、设置方向</span></a>"
              "<a href='#reminders'>提醒事项<span>便签、待办、闹钟与计时器</span></a>"
              "<a href='#voice'>语音与音量<span>调整音色、唤醒名字和音量</span></a>"
              "<a href='#personal'>个人记忆<span>管理偏好和云端备份</span></a>"
              "<a href='#focus'>番茄钟<span>开始专注、暂停和休息</span></a>"
              "<a href='#network'>联网与天气<span>选择 Wi-Fi 和天气地区</span></a></div>"
              "<div class='card' style='margin-top:20px'><h3>设备概况</h3><p id='home-stage'>正在读取状态…</p>");
    if (WiFi.status() == WL_CONNECTED) {
      page += "<p class='ok'>Wi-Fi 已连接：" + htmlEscape(WiFi.SSID()) +
              "，IP：<code>" + WiFi.localIP().toString() + "</code></p>";
    } else if (configPortalActive) {
      page += "<p class='warn'>当前处于配网模式。热点：<code>" +
              htmlEscape(configApSsid) + "</code>，配置地址：<code>" +
              WiFi.softAPIP().toString() + "</code></p>";
    } else {
      page += F("<p class='warn'>Wi-Fi 当前未连接。</p>");
    }
    page += "<p>当前云线路：<code>" +
            htmlEscape(activeServerUrl.isEmpty() ? String("尚未检测") : activeServerUrl) + "</code></p>";
    page += settings.deviceToken.isEmpty()
                ? F("<p class='warn'>尚未配置设备 Token，当前不能发起对话。</p>")
                : F("<p class='ok'>设备 Token 已配置（不会在网页回显）。</p>");
    page += "<p>固件：<code>" + String(FIRMWARE_VERSION) + "</code>，OTA：<code>" +
            htmlEscape(otaStatus) + "</code> " + htmlEscape(otaMessage) + "</p>";
    page += F("<div class='actions'><button type='button' data-photo-action onclick=\"selectStandbyPhoto('previous')\">上一张图片</button>"
              "<button type='button' data-photo-action onclick=\"selectStandbyPhoto('next')\">下一张图片</button>"
              "<form method='post' action='/display/resume'><button class='secondary' type='submit'>恢复待机轮播</button></form></div>"
              "<p id='home-action' role='status'></p></div></section>"
              "<section class='panel' data-panel='diagnostics' hidden><h2>运行记录</h2><p class='intro'>对话记录与排查信息；只在查看时刷新详细日志。</p>");
    page += F("<fieldset><legend>实时对话流程</legend><p id='flow-summary'>正在读取运行状态…</p>"
              "<p id='flow-cloud'></p>"
              "<p id='flow-error' class='warn'></p><pre id='flow-events' style='white-space:pre-wrap;"
              "background:#101828;color:#d0f0df;padding:12px;border-radius:6px;max-height:320px;overflow:auto'>"
              "</pre><p><small>查看此页时每 3 秒刷新；记录联网、证书、鉴权、上传、分段等待和播放错误。"
              "Token、Wi-Fi 密码不会进入日志。</small></p></fieldset>");
    page += F("<fieldset><legend>当前对话</legend><div id='current-chat' class='chat'>等待新一轮对话…</div>"
              "<p><small>这里只显示当前正在处理/播放的话，不累计整段回答。</small></p></fieldset>"
              "<fieldset><legend>历史记录</legend><div id='history-chat' class='chat'>正在读取历史记录…</div>"
              "<p><small>每轮完成后只保存一次完整的‘你 / AI’记录；达到约 24 KB 后自动从新记录开始。</small></p>"
              "<form method='post' action='/conversation/clear'><label><input type='checkbox' name='confirm' required> 确认清空本机聊天记录</label>"
              "<label><input type='checkbox' name='cloud_memory'> 同时清空本设备的服务器对话记忆</label>"
              "<button type='submit' class='danger'>清空聊天记录</button></form></fieldset>");
    page += F("</section><section class='panel' data-panel='display' hidden><h2>屏幕与相册</h2><p class='intro'>管理照片，或选择日常待机页面。</p>");
    DisplayWeb::appendControls(page, settings.volumePotentiometer);
    page += F("<details><summary>待机页面、屏幕型号与旋转方向</summary><fieldset><legend>待机页面显示</legend><form method='post' action='/display/pages'>"
              "<p>勾选要启用的页面；选择多项时自动轮播，只选择一项时固定显示。</p>"
              "<label><input style='width:auto' type='checkbox' name='expression' value='1'");
    if (settings.displayPages & RobotDisplay::PAGE_EXPRESSION) page += F(" checked");
    page += F("> 表情模式：横屏大眼睛，竖屏全身动作，随机切换情绪</label>"
              "<label><input style='width:auto' type='checkbox' name='home' value='1'");
    if (settings.displayPages & RobotDisplay::PAGE_HOME) page += F(" checked");
    page += F("> 主界面：动态表情、时间、当前天气、地区滚动、下一闹钟</label>"
              "<label><input style='width:auto' type='checkbox' name='forecast' value='1'");
    if (settings.displayPages & RobotDisplay::PAGE_FORECAST) page += F(" checked");
    page += F("> 三天预报：天气图标、温度范围</label>"
              "<label><input style='width:auto' type='checkbox' name='memos' value='1'");
    if (settings.displayPages & RobotDisplay::PAGE_MEMOS) page += F(" checked");
    page += F("> 便签与待办：最近内容、未完成数量</label>"
              "<label><input style='width:auto' type='checkbox' name='photo' value='1'");
    if (settings.displayPages & RobotDisplay::PAGE_PHOTO) page += F(" checked");
    page += F("> 图片 / 照片：显示下方上传的图片</label>"
              "<p><button type='submit'>保存显示方式</button></p>"
              "<p><small>至少保留一个页面；设置在重启和 OTA 后继续生效。</small></p></form>"
              "<hr><form method='post' action='/display/panel'><label>当前连接的屏幕型号</label>"
              "<select name='panel'><option value='st7735'");
    if (RobotDisplay::currentPanel() == RobotDisplay::Panel::ST7735S_128X160_8PIN) page += F(" selected");
    page += F(">ST7735S 8P（128×160，横屏 160×128）</option><option value='st7789'");
    if (RobotDisplay::currentPanel() == RobotDisplay::Panel::M200T_ST7789_240X320_7PIN) page += F(" selected");
    page += F(">M200T / ST7789 7P（240×320，横屏 320×240）</option></select>"
              "<p><button type='submit'>保存屏幕型号并重启</button></p>"
              "<p><small>换屏后在这里选择对应型号。保存后设备约 2 秒自动重启；请勿带电插拔屏幕。</small></p></form>"
              "<hr><form method='post' action='/display/rotation'><label>屏幕旋转方向</label><select name='rotation'>");
    const char *rotationLabels[] = {"0°（竖屏）", "90°（横屏，默认）", "180°（反向竖屏）", "270°（反向横屏）"};
    for (uint8_t rotation = 0; rotation < 4; ++rotation) {
      page += String("<option value='") + String(rotation * 90) + "'";
      if (RobotDisplay::currentRotation() == rotation) page += F(" selected");
      page += String(">") + rotationLabels[rotation] + "</option>";
    }
    page += F("</select><p><button type='submit'>保存方向并重启</button></p>"
              "<p><small>方向按屏幕原生竖屏计算；横竖布局和照片自动适配。重启、OTA 后保留。</small></p></form></fieldset>");
    page += F("</details>");
    page += FPSTR(PHOTO_SETTINGS_PAGE);
    page += F("</section><section class='panel' data-panel='reminders' hidden><h2>提醒事项</h2>");
    MemoWeb::append(page, htmlEscape);
    page += F("<fieldset id='alarms'><legend>闹钟和计时器</legend><p>");
    page += htmlEscape(AlarmManager::summary(time(nullptr)));
    page += F("</p>");
    AlarmManager::Entry alarmEntry;
    for (size_t alarmIndex = 0; alarmIndex < AlarmManager::count(); ++alarmIndex) {
      if (!AlarmManager::get(alarmIndex, alarmEntry)) continue;
      page += F("<div class='alarm-row'><strong>#");
      page += String(alarmEntry.id);
      page += F("</strong> ");
      page += htmlEscape(alarmEntryText(alarmEntry));
      page += alarmEntry.enabled ? F(" <span class='ok'>已启用</span>") : F(" <span class='warn'>已停用</span>");
      page += F("<div class='actions'><form method='post' action='/alarms/delete'><input type='hidden' name='id' value='");
      page += String(alarmEntry.id);
      page += F("'><button class='danger' type='submit'>删除</button></form></div></div>");
    }
    if (AlarmManager::count() == 0) page += F("<p class='meta'>尚未设置闹钟。</p>");
    AlarmMusicWeb::appendControls(page);
    page += F("<form method='post' action='/alarms/add'><div class='row'><div><label>时间</label>"
              "<input type='time' name='time' required></div><div><label>重复</label><select name='days'>"
              "<option value='daily'>每天</option><option value='workdays'>工作日</option>"
              "<option value='weekend'>周末</option></select></div></div><label>名称</label>"
              "<input name='label' maxlength='24' value='闹钟'>"
              "<p><button type='submit'>添加闹钟</button></p></form>"
              "<form method='post' action='/alarms/timer'><div class='row'><div><label>计时时长</label>"
              "<input type='number' name='duration' min='1' max='10080' required></div><div><label>单位</label>"
              "<select name='unit'><option value='minutes'>分钟</option><option value='hours'>小时</option>"
              "<option value='seconds'>秒</option></select></div></div>"
              "<p><button type='submit'>开始计时</button></p></form>"
              "<p><small>也可直接说“每天早上七点设置闹钟”“十分钟计时”“取消下一个闹钟”或“查看所有闹钟”。配置写入 NVS，断电后仍保留；断电期间设备本身无法响铃。</small></p></fieldset>");
    page += F("</section><section class='panel' data-panel='network' hidden><h2>联网与天气</h2><p class='intro'>Wi-Fi 与天气分别保存，互不影响。</p>");
    appendWifiPage(page);
    page += F("</section><section class='panel' data-panel='system' hidden><h2>设备与更新</h2><p class='intro'>连接信息和固件维护。</p><details><summary>云端连接与登录密码</summary>");
    page += F("<form method='post' action='/save'><fieldset><legend>云端连接与登录密码</legend>"
              "<label>云服务器地址</label><input name='server_url' maxlength='180' value='");
    page += htmlEscape(settings.serverUrl);
    page += F("'><label>设备 ID</label><input name='device_id' maxlength='64' value='");
    page += htmlEscape(settings.deviceId);
    page += F("'><label>设备 Token（留空保持原值）</label><input type='password' name='device_token' maxlength='256'>"
              "<label>新设备连接码（留空保持原值）</label><input type='password' inputmode='numeric' name='admin_pin' minlength='6' maxlength='6' pattern='[0-9]{6}' placeholder='6 位数字'>"
              "<p><small>首次默认连接码为 123456；修改后立即写入 NVS，无需重启。</small></p>"
              "<p><button type='submit'>保存设备连接</button></p></fieldset></form></details></section>"
              "<section class='panel' data-panel='voice' hidden><h2>语音与音量</h2><p class='intro'>选择音色、唤醒词和适合的音量。</p><form method='post' action='/save'><fieldset><legend>本设备个性化语音</legend><div class='row'><div>"
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
    if (RobotDisplay::potentiometerBrightnessEnabled() || RobotDisplay::BATTERY_MONITOR_ENABLED) page += F(" disabled");
    page += F(">GPIO1 电位器</option></select><label>离线中文唤醒</label><select name='offline_wake_enabled'><option value='on'");
    if (settings.offlineWakeEnabled) page += F(" selected");
    page += F(">开启</option><option value='off'");
    if (!settings.offlineWakeEnabled) page += F(" selected");
    page += F(">关闭（说话键仍可用）</option></select><label>唤醒名字</label><select name='offline_wake_model'>");
    for (size_t wakeIndex = 0; wakeIndex < OfflineWake::modelOptionCount(); ++wakeIndex) {
      const OfflineWake::ModelOption &option = OfflineWake::modelOption(wakeIndex);
      page += "<option value='" + String(option.id) + "'";
      if (settings.offlineWakeModel == option.id) page += F(" selected");
      page += ">" + htmlEscape(option.label) + "</option>";
    }
    page += F("</select><p><button type='submit'>保存语音设置</button></p></fieldset></form>");
    {
      String captureControls;
      VoiceWeb::appendControls(captureControls);
      MultiMicWeb::appendControls(captureControls);
      page += captureControls;
    }
    DialogueWeb::appendControls(page);
    WebCustomWake::appendControls(page);
    InteractionWeb::appendControls(page);
    UsageWeb::append(page);
    page += F("</section><section class='panel' data-panel='personal' hidden>");
    ProfileWeb::appendControls(page);
    page += F("</section><section class='panel' data-panel='focus' hidden>");
    FocusWeb::appendControls(page);
    page += F("</section><section class='panel' data-panel='network' hidden><form method='post' action='/save'><fieldset><legend>天气地区</legend>"
               "<label>高德 Web 服务 Key（留空保持原值）</label><input type='password' name='amap_key' maxlength='64'>");
    page += settings.amapKey.isEmpty()
                ? F("<p class='warn'>尚未配置高德 Key。</p>")
                : F("<p class='ok'>高德 Key 已配置（不会在网页或状态接口回显）。</p>");
    page += F("<label>搜索城市、区县、地址或地标</label><input id='weather-query' maxlength='64' placeholder='例如：上海市静安区、虹桥站'>"
              "<div class='weather-actions'><button type='button' onclick='searchWeather()'>搜索地址</button>"
              "<button type='button' onclick='locateWeatherIp()'>按公网 IP 定位</button></div>"
              "<div id='weather-results' class='weather-result'></div><p id='weather-selected'>当前：");
    page += htmlEscape(settings.weatherCity) + "（adcode " + htmlEscape(settings.weatherAdcode) + "，" +
            (settings.weatherLocationMode == "ip" ? "IP 自动定位" : "手动选择") + "）</p>";
    page += F("<input type='hidden' id='weather-city' name='weather_city' value='");
    page += htmlEscape(settings.weatherCity);
    page += F("'><input type='hidden' id='weather-adcode' name='weather_adcode' value='");
    page += htmlEscape(settings.weatherAdcode);
    page += F("'><input type='hidden' id='weather-mode' name='weather_location_mode' value='");
    page += htmlEscape(settings.weatherLocationMode);
    page += F("'><input type='hidden' id='weather-latitude' name='weather_latitude' value='");
    page += String(settings.weatherLatitude, 6);
    page += F("'><input type='hidden' id='weather-longitude' name='weather_longitude' value='");
    page += String(settings.weatherLongitude, 6);
    page += F("'><div class='row'><div><label>已解析纬度</label><input id='weather-latitude-view' readonly value='");
    page += String(settings.weatherLatitude, 4);
    page += F("'></div><div><label>已解析经度</label><input id='weather-longitude-view' readonly value='");
    page += String(settings.weatherLongitude, 4);
    page += F("'></div></div><p><small>先搜索并点选结果，或使用 IP 定位，再保存配置；天气按 adcode 每 30 分钟刷新。IP 模式会在重启后重新定位。</small></p>"
              "<p><button type='submit'>保存天气地区</button></p></fieldset></form></section>"
              "<section class='panel' data-panel='system' hidden><form method='post' action='/save'><fieldset><legend>线上更新策略</legend>"
              "<label>检查到新版本后</label><select name='ota_policy'><option value='manual'");
    if (!settings.otaAutoUpdate) page += F(" selected");
    page += F(">等待网页确认安装</option><option value='auto'");
    if (settings.otaAutoUpdate) page += F(" selected");
    page += F(">凌晨自动下载、校验并安装</option></select><p><small>设备仅响应手动更新，或每天凌晨 03:00～03:09 检查一次；暂缓版本不会安装。</small></p>"
              "<p><button type='submit'>保存更新策略</button></p></fieldset></form>"
              "<fieldset><legend>线上固件升级</legend>"
              "<p>设备只通过 HTTPS 获取经过 RSA 签名且 SHA-256 匹配的固件。升级安装期间请保持供电。</p>"
              "<p>可用版本：<code>");
    page += htmlEscape(otaAvailableVersion.isEmpty() ? String("尚未检查") : otaAvailableVersion);
    page += F("</code>；暂缓版本：<code>");
    page += htmlEscape(settings.otaDeferredVersion.isEmpty() ? String("无") : settings.otaDeferredVersion);
    page += F("</code></p>"
              "<div class='actions'><form method='post' action='/ota/check'><button type='submit'>只检查更新</button></form>"
              "<form method='post' action='/ota/install'><button type='submit'>立即安装可用更新</button></form>"
              "<form method='post' action='/ota/defer'><button type='submit'>暂不更新当前版本</button></form></div>"
              "</fieldset>"
              "<details><summary>开发与诊断</summary><a href='/status'>查看完整 JSON 状态</a></details></section>"
              "<script>const labels={booting:'启动',idle:'待机',checking_network:'检查网络',"
              "wifi_connecting:'连接Wi-Fi',wifi_connected:'Wi-Fi已连接',time_ready:'时间已同步',"
              "time_error:'时间同步失败',server_ready:'HTTPS就绪',server_fallback:'HTTP回退',"
              "server_error:'服务器不可用',recording:'录音',recorded:'录音完成',uploading:'上传录音',upload_retry:'上传重试',"
              "audio_suspended:'音频已释放',audio_suspend_error:'音频释放失败',"
              "waiting_segment:'等待语音分段',playing:'播放',conversation_text:'同步对话文字',completed:'完成',no_speech:'未检测到说话',cancelled:'已结束对话',error:'失败'};"
              "function esc(s){return String(s||'').replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]));}"
              "function selectWeather(x,mode){document.getElementById('weather-city').value=x.name;document.getElementById('weather-adcode').value=x.adcode;document.getElementById('weather-mode').value=mode;const lat=Number(x.latitude||0),lon=Number(x.longitude||0);document.getElementById('weather-latitude').value=lat;document.getElementById('weather-longitude').value=lon;document.getElementById('weather-latitude-view').value=lat.toFixed(4);document.getElementById('weather-longitude-view').value=lon.toFixed(4);document.getElementById('weather-selected').textContent='已选择：'+x.name+'（adcode '+x.adcode+'，'+(mode==='ip'?'IP自动定位':'手动选择')+'）';document.getElementById('weather-results').textContent='';}"
              "function renderWeatherItems(items){const box=document.getElementById('weather-results');box.textContent='';for(const x of items){const b=document.createElement('button');b.type='button';b.textContent=x.name+' ｜ '+(x.district||'')+' ｜ '+x.adcode;b.onclick=()=>selectWeather(x,'manual');box.appendChild(b);}}"
              "async function weatherFetch(path){const r=await adminFetch(path);const text=await r.text();if(!r.ok)throw new Error(text);return JSON.parse(text);}"
              "async function searchWeather(){const box=document.getElementById('weather-results');try{const q=document.getElementById('weather-query').value.trim();if(!q)throw new Error('请输入地址关键词');box.textContent='正在搜索…';const d=await weatherFetch('/weather/search?q='+encodeURIComponent(q));renderWeatherItems(d.items||[]);}catch(e){box.textContent='搜索失败：'+e.message;}}"
              "async function locateWeatherIp(){const box=document.getElementById('weather-results');try{box.textContent='正在按IP定位…';const d=await weatherFetch('/weather/ip');selectWeather(d.item,'ip');}catch(e){box.textContent='IP定位失败：'+e.message;}}"
              "const chatTimeFormat=new Intl.DateTimeFormat('sv-SE',{timeZone:'Asia/Shanghai',year:'numeric',month:'2-digit',day:'2-digit',hour:'2-digit',minute:'2-digit',second:'2-digit',hourCycle:'h23'});"
              "function chatTime(value){if(value===undefined)return '时间未记录';const n=Number(value);if(!Number.isFinite(n)||n<1700000000||n>8640000000000)return '时间未同步';return chatTimeFormat.format(new Date(n*1000));}"
              "function bubble(role,text,label,at){if(!text)return '';return '<div class=\"msg '+role+'\"><div class=\"meta\">'+esc(label)+' · <span title=\"北京时间\">'+esc(chatTime(at))+'</span></div>'+esc(text)+'</div>';}"
              "async function refreshConversation(d){try{let c='';c+=bubble('user',d.current_user_text,'你 · 当前',d.current_user_at);c+=bubble('assistant',d.current_assistant_text,'AI · 当前',d.current_assistant_at);document.getElementById('current-chat').innerHTML=c||'<span class=\"meta\">等待新一轮对话…</span>';const r=await adminFetch('/conversation.log');let h='';if(r.ok){const t=await r.text();for(const line of t.trim().split('\\n')){if(!line)continue;try{const x=JSON.parse(line);h+=bubble('user',x.user,'你',x.user_at);h+=bubble('assistant',x.assistant,'AI',x.assistant_at);}catch(_){}}}document.getElementById('history-chat').innerHTML=h||'<span class=\"meta\">暂无历史记录。</span>';}catch(e){document.getElementById('history-chat').textContent='对话记录读取失败：'+e;}}"
              "async function refreshFlow(){if(document.hidden||standbyPhotoBusy||!['home','diagnostics','voice'].includes(window.activeAdminPanel||'home')){setTimeout(refreshFlow,3000);return;}try{const voicePanel=window.activeAdminPanel==='voice';const r=await adminFetch(voicePanel?'/status':'/runtime');"
              "const d=await r.json();if(voicePanel){window.updateVoiceCaptureStatus(d);}else{const s=labels[d.stage]||d.stage;document.getElementById('home-stage').textContent='当前状态：'+s;"
              "document.getElementById('flow-summary').textContent='阶段：'+s+' ｜ HTTP：'+"
              "(d.last_http_code||'-')+' ｜ 分段：'+d.segment_index+' ｜ Wi-Fi：'+d.wifi+"
              "' '+d.rssi+' dBm ｜ 堆：'+d.free_heap+' ｜ 最大连续块：'+d.largest_free_block+"
              "' ｜ 历史最低：'+d.minimum_free_heap;"
              "document.getElementById('flow-cloud').textContent='云连接：'+"
              "(d.cloud_keepalive?'保持中':'未保持')+' ｜ 空闲：'+(d.cloud_idle_ms||0)+"
              "' ms ｜ 探测：'+(d.active_server_url||'-')+' ｜ 对话：'+(d.dialogue_base_url||'-');"
              "document.getElementById('flow-error').textContent=d.last_error?'最近错误：'+d.last_error:'';"
              "document.getElementById('flow-events').textContent=d.events.map(e=>'['+(e.at_ms/1000).toFixed(1)+'s] '+e.text).join('\\n');"
              "if(window.activeAdminPanel==='diagnostics')await refreshConversation(d);}"
              "}catch(e){document.getElementById('flow-error').textContent='状态读取失败：'+e;}"
              "setTimeout(refreshFlow,3000)}refreshFlow();</script>");
    page += FPSTR(EXPRESSION_SETTINGS_PAGE);
    MusicWeb::append(page);
    page += FPSTR(ADMIN_NAV_SCRIPT);
    page.finish();
  });

  phoneWeb.on("/weather/search", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (WiFi.status() != WL_CONNECTED) {
      phoneWeb.send(503, "text/plain; charset=utf-8",
                    "设备尚未连入互联网；请先保存正确的 Wi-Fi 配置。");
      return;
    }
    const String query = phoneWeb.arg("q");
    if (query.isEmpty() || query.length() > 64) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "地址关键词长度无效。");
      return;
    }
    if (busy || weatherRefreshRunning) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在处理任务，请稍后搜索。");
      return;
    }
    String output;
    String errorMessage;
    if (!amapSearchLocations(query, output, errorMessage)) {
      phoneWeb.send(502, "text/plain; charset=utf-8", errorMessage);
      return;
    }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", output);
  });

  phoneWeb.on("/weather/ip", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (WiFi.status() != WL_CONNECTED) {
      phoneWeb.send(503, "text/plain; charset=utf-8",
                    "设备尚未连入互联网；请先保存正确的 Wi-Fi 配置。");
      return;
    }
    if (busy || weatherRefreshRunning) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在处理任务，请稍后定位。");
      return;
    }
    String output;
    String errorMessage;
    if (!amapIpLocation(output, errorMessage)) {
      phoneWeb.send(502, "text/plain; charset=utf-8", errorMessage);
      return;
    }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", output);
  });

  phoneWeb.on("/display/brightness", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingDisplayRestartAtMs || pendingOtaCheck || pendingOtaInstall) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在忙，请稍后保存亮度。"); return;
    }
    const String value = phoneWeb.arg("brightness");
    const String mode = phoneWeb.arg("mode");
    bool valid = value.length() > 0 && value.length() <= 3;
    for (size_t i = 0; valid && i < value.length(); ++i) valid = value[i] >= '0' && value[i] <= '9';
    const int percent = value.toInt();
    if (!valid || percent < RobotDisplay::MIN_BRIGHTNESS || percent > 100 ||
        (mode != "fixed" && mode != "pot")) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "亮度应为 5–100 的整数，并选择网页或电位器模式。"); return;
    }
    if (mode == "pot" && (settings.volumePotentiometer || RobotDisplay::BATTERY_MONITOR_ENABLED)) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "GPIO1 已被音量或电池检测占用，请先关闭对应功能。"); return;
    }
    if (!RobotDisplay::saveBrightnessPreference(percent, mode == "pot")) {
      phoneWeb.send(500, "text/plain; charset=utf-8", "亮度保存失败，请重试。"); return;
    }
    phoneWeb.sendHeader("Location", "/#display"); phoneWeb.send(303);
  });

  phoneWeb.on("/display/voice-layout", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingDisplayRestartAtMs || pendingOtaCheck || pendingOtaInstall) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在忙，请稍后保存显示方式。"); return;
    }
    const String button = phoneWeb.arg("button"), wake = phoneWeb.arg("wake");
    if ((button != "compact" && button != "full") || (wake != "compact" && wake != "full")) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "请分别选择按键和唤醒的显示方式。"); return;
    }
    if (!RobotDisplay::saveVoiceLayoutPreferences(button == "full", wake == "full")) {
      phoneWeb.send(500, "text/plain; charset=utf-8", "显示方式保存失败，请重试。"); return;
    }
    phoneWeb.sendHeader("Location", "/#display"); phoneWeb.send(303);
  });

  phoneWeb.on("/display/appearance", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingDisplayRestartAtMs || pendingOtaCheck || pendingOtaInstall) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在忙，请稍后保存造型。"); return;
    }
    const String style = phoneWeb.arg("style");
    if (style != "face" && style != "body") {
      phoneWeb.send(422, "text/plain; charset=utf-8", "请选择有效造型。"); return;
    }
    if (!RobotDisplay::savePortraitBodyPreference(style == "body")) {
      phoneWeb.send(500, "text/plain; charset=utf-8", "造型保存失败，请重试。"); return;
    }
    phoneWeb.sendHeader("Location", "/#display"); phoneWeb.send(303);
  });

  phoneWeb.on("/display/expressions", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const uint64_t enabled = RobotDisplay::expressionMask();
    if (!enabled) {
      phoneWeb.send(503, "text/plain; charset=utf-8", "表情库暂不可用，请稍后重试。"); return;
    }
    ExpressionWeb::sendCatalog(phoneWeb, enabled);
  });

  phoneWeb.on("/display/expressions", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingDisplayRestartAtMs || pendingOtaCheck || pendingOtaInstall) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在忙，请稍后保存表情。"); return;
    }
    uint64_t enabled = 0;
    if (!RobotExpressions::parseSelection(phoneWeb.arg("ids"), enabled)) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "请至少选择一组有效表情，不能重复。"); return;
    }
    if (!RobotDisplay::saveExpressionMask(enabled)) {
      phoneWeb.send(500, "text/plain; charset=utf-8", "表情选择保存失败，请重试。"); return;
    }
    phoneWeb.send(200, "text/plain; charset=utf-8", "已保存");
  });

  phoneWeb.on("/display/expressions/image", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    uint8_t id;
    const String body = phoneWeb.arg("body");
    if (!RobotExpressions::parseId(phoneWeb.arg("id"), id) || (body != "0" && body != "1")) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "表情或预览样式无效。"); return;
    }
    if (busy || pendingDisplayRestartAtMs || pendingOtaInstall) {
      phoneWeb.send(503, "text/plain; charset=utf-8", "设备正在忙，请稍后读取缩略图。"); return;
    }
    ExpressionWeb::sendThumbnail(phoneWeb, id, body == "1");
  });

  phoneWeb.on("/display/expressions/preview", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (phoneWeb.arg("id") == "stop") {
      RobotDisplay::cancelExpressionPreview();
      phoneWeb.send(200, "text/plain; charset=utf-8", "已结束预览"); return;
    }
    uint8_t id;
    if (!RobotExpressions::parseId(phoneWeb.arg("id"), id)) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "表情无效。"); return;
    }
    if (busy || pendingDisplayRestartAtMs || pendingOtaCheck || pendingOtaInstall ||
        !RobotDisplay::previewExpression(id)) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备当前不能预览，请在待机时重试。"); return;
    }
    phoneWeb.send(200, "text/plain; charset=utf-8", "正在预览");
  });

  phoneWeb.on("/display/pages", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) {
      phoneWeb.send(409, "text/html; charset=utf-8",
                    autoReturnPage("显示方式未保存", "设备正在回答，请稍后重试。", 5));
      return;
    }
    uint8_t pageMask = 0;
    if (phoneWeb.arg("home") == "1") pageMask |= RobotDisplay::PAGE_HOME;
    if (phoneWeb.arg("forecast") == "1") pageMask |= RobotDisplay::PAGE_FORECAST;
    if (phoneWeb.arg("memos") == "1") pageMask |= RobotDisplay::PAGE_MEMOS;
    if (phoneWeb.arg("expression") == "1") pageMask |= RobotDisplay::PAGE_EXPRESSION;
    if (phoneWeb.arg("photo") == "1") pageMask |= RobotDisplay::PAGE_PHOTO;
    if (pageMask == 0) {
      phoneWeb.send(422, "text/html; charset=utf-8",
                    autoReturnPage("显示方式未保存", "至少需要启用一个待机页面。", 5));
      return;
    }
    const uint8_t previousPages = settings.displayPages;
    settings.displayPages = pageMask;
    if (!saveSettings()) {
      settings.displayPages = previousPages;
      saveSettings();
      phoneWeb.send(500, "text/html; charset=utf-8",
                    autoReturnPage("显示方式未保存", "NVS 写入失败。", 5));
      return;
    }
    RobotDisplay::setPageMask(settings.displayPages);
    RobotDisplay::setPhotoMode(false);
    uint8_t enabledCount = 0;
    for (uint8_t bit = RobotDisplay::PAGE_HOME; bit <= RobotDisplay::PAGE_PHOTO; bit <<= 1) {
      if (settings.displayPages & bit) ++enabledCount;
    }
    traceRuntime("display_pages_saved",
                 enabledCount > 1 ? "多个待机页面自动轮播" : "固定显示所选待机页面");
    returnToHomeDisplay();
    phoneWeb.send(200, "text/html; charset=utf-8",
                  autoReturnPage("显示方式已保存",
                                 enabledCount > 1 ? "已启用多个页面并自动轮播。"
                                                  : "已固定显示所选页面。",
                                 4));
  });

  phoneWeb.on("/display/panel", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) {
      phoneWeb.send(409, "text/html; charset=utf-8",
                    autoReturnPage("屏幕型号未保存", "设备正在回答，请稍后重试。", 5));
      return;
    }
    RobotDisplay::Panel selected;
    if (!RobotDisplay::panelFromId(phoneWeb.arg("panel"), selected)) {
      phoneWeb.send(422, "text/html; charset=utf-8",
                    autoReturnPage("屏幕型号未保存", "不支持的屏幕型号。", 5));
      return;
    }
    if (!RobotDisplay::savePanelPreference(selected)) {
      phoneWeb.send(500, "text/html; charset=utf-8",
                    autoReturnPage("屏幕型号未保存", "NVS 写入失败。", 5));
      return;
    }
    const bool changed = selected != RobotDisplay::currentPanel();
    traceRuntime("display_panel_saved",
                 String("屏幕型号已保存：") + RobotDisplay::panelLabel(selected) +
                     (changed ? "，等待重启生效" : "，当前已生效"));
    if (changed) pendingDisplayRestartAtMs = millis() + 1800UL;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8",
                  autoReturnPage("屏幕型号已保存",
                                 changed ? "设备将在约 2 秒后自动重启并按新屏幕初始化。"
                                         : "当前已经是这个屏幕型号，无需重启。",
                                 changed ? 8 : 4));
  });

  phoneWeb.on("/display/resume", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) { phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在忙，请稍后重试。"); return; }
    RobotDisplay::setPhotoMode(false);
    returnToHomeDisplay();
    phoneWeb.sendHeader("Location", "/#display"); phoneWeb.send(303);
  });

  phoneWeb.on("/display/rotation", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingDisplayRestartAtMs != 0 || pendingOtaCheck || pendingOtaInstall) {
      phoneWeb.send(409, "text/html; charset=utf-8",
                    autoReturnPage("屏幕方向未保存", "设备正在忙或准备重启，请稍后重试。", 5));
      return;
    }
    const String requested = phoneWeb.arg("rotation");
    if (requested != "0" && requested != "90" && requested != "180" && requested != "270") {
      phoneWeb.send(422, "text/html; charset=utf-8",
                    autoReturnPage("屏幕方向未保存", "请选择 0、90、180 或 270 度。", 5));
      return;
    }
    const uint8_t rotation = requested.toInt() / 90;
    if (!RobotDisplay::saveRotationPreference(rotation)) {
      phoneWeb.send(500, "text/html; charset=utf-8",
                    autoReturnPage("屏幕方向未保存", "NVS 写入失败。", 5));
      return;
    }
    const bool changed = rotation != RobotDisplay::currentRotation();
    if (changed) pendingDisplayRestartAtMs = millis() + 1800UL;
    traceRuntime("display_rotation_saved", String("屏幕方向已保存：") + requested + " 度");
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8",
                  autoReturnPage("屏幕方向已保存",
                                 changed ? "设备将在约 2 秒后重启，自动应用横竖布局。"
                                         : "当前已经是这个方向，无需重启。", changed ? 8 : 4));
  });

  phoneWeb.on("/memos", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(MemoManager::ready() ? 200 : 503,
                  "application/json; charset=utf-8", MemoManager::toJson());
  });

  phoneWeb.on("/memos/add", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const String text = phoneWeb.arg("text");
    if (text.isEmpty() || text.length() > MemoManager::MAX_TEXT_BYTES) {
      phoneWeb.send(422, "text/html; charset=utf-8",
                    autoReturnPage("未添加", "内容不能为空且不能超过 180 字节。", 5));
      return;
    }
    const MemoManager::Kind kind = phoneWeb.arg("kind") == "todo"
                                       ? MemoManager::Kind::Todo
                                       : MemoManager::Kind::Note;
    const bool ok = MemoManager::add(kind, text, time(nullptr));
    refreshMemoDisplay();
    phoneWeb.send(ok ? 200 : 422, "text/html; charset=utf-8",
                  autoReturnPage(ok ? "已添加" : "未添加",
                                 ok ? MemoManager::summary()
                                    : "存储失败或便签与待办总数已达到 20 条。",
                                 5));
  });

  phoneWeb.on("/memos/toggle", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const uint32_t id = static_cast<uint32_t>(phoneWeb.arg("id").toInt());
    const bool completed = phoneWeb.arg("completed") == "1";
    const bool ok = MemoManager::setCompleted(id, completed);
    refreshMemoDisplay();
    phoneWeb.send(ok ? 200 : 404, "text/html; charset=utf-8",
                  autoReturnPage(ok ? "待办已更新" : "状态未更新",
                                 ok ? (completed ? "已标记为完成。" : "已恢复为未完成。")
                                    : "没有找到对应的待办编号或存储失败。",
                                 5));
  });

  phoneWeb.on("/memos/delete", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const bool ok = MemoManager::remove(static_cast<uint32_t>(phoneWeb.arg("id").toInt()));
    refreshMemoDisplay();
    phoneWeb.send(ok ? 200 : 404, "text/html; charset=utf-8",
                  autoReturnPage(ok ? "已删除" : "未删除",
                                 ok ? MemoManager::summary() : "没有找到该编号或存储失败。", 5));
  });

  phoneWeb.on("/memos/clear", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const String scope = phoneWeb.arg("scope");
    const size_t removed = scope == "notes"
                               ? MemoManager::clear(MemoManager::Kind::Note)
                               : scope == "todos"
                                     ? MemoManager::clear(MemoManager::Kind::Todo)
                                     : MemoManager::clearCompletedTodos();
    refreshMemoDisplay();
    phoneWeb.send(200, "text/html; charset=utf-8",
                  autoReturnPage("清理完成", removed > 0 ? "已删除 " + String(removed) + " 条。"
                                                        : "没有符合条件的内容。",
                                 5));
  });

  phoneWeb.on("/alarms", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8",
                  AlarmManager::toJson(time(nullptr)));
  });

  phoneWeb.on("/voice/noise/reset", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || AlarmManager::ringing() || conversation.active()) { phoneWeb.send(409, "text/plain", "请在待机时校准"); return; }
    VoiceInput::resetAmbientNoise();
    phoneWeb.send(200, "application/json", "{\"ok\":true}");
  });
  phoneWeb.on("/alarm/dismiss", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    AlarmManager::dismiss(); VoiceInput::muteAmplifier(true);
    phoneWeb.send(200, "application/json", "{\"ok\":true}");
  });
  phoneWeb.on("/alarm/snooze", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const bool ok = AlarmManager::snooze(time(nullptr), 300);
    VoiceInput::muteAmplifier(true);
    phoneWeb.send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"error\":\"稍后提醒未保存\"}");
  });
  phoneWeb.on("/alarm/music", HTTP_ANY, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || conversation.active() || AlarmManager::ringing() || pendingWebRecording) { phoneWeb.send(409, "text/plain", "请先结束当前对话或闹钟"); return; }
    if (phoneWeb.method() != HTTP_GET && phoneWeb.method() != HTTP_POST) { phoneWeb.send(405); return; }
    const bool save = phoneWeb.method() == HTTP_POST;
    if (!beginCloudRequest(requestBaseUrl() + (save ? "/voice-api/v1/alarm-music/selection" : "/voice-api/v1/alarm-music"))) { phoneWeb.send(503); return; }
    addAuthHeaders(cloudHttp); cloudHttp.setTimeout(6000);
    if (save) cloudHttp.addHeader("Content-Type", "application/json");
    const int code = save ? cloudHttp.PUT(phoneWeb.arg("plain")) : cloudHttp.GET();
    const String response = code > 0 && cloudHttp.getSize() <= 8192 ? cloudHttp.getString() : "服务器连接失败";
    endCloudRequest(); phoneWeb.send(code > 0 ? code : 502, "application/json; charset=utf-8", response);
  });
  phoneWeb.on("/alarm/music/upload", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    alarmMusicUpload.close();
    if (alarmMusicUploadFailed || !alarmMusicUploadBytes) { phoneWeb.send(422, "text/plain", "上传失败：请使用15秒以内的音乐，并在待机时上传"); return; }
    File file = VoiceInput::recordingStorage().open("/alarm-upload.wav", FILE_READ);
    if (!file || !beginCloudRequest(requestBaseUrl() + "/voice-api/v1/alarm-music-upload?title=" + urlEncodeComponent(alarmMusicUploadTitle))) {
      file.close(); VoiceInput::recordingStorage().remove("/alarm-upload.wav"); phoneWeb.send(503); return;
    }
    addAuthHeaders(cloudHttp); cloudHttp.setTimeout(10000); cloudHttp.addHeader("Content-Type", "audio/wav");
    const int code = cloudHttp.sendRequest("POST", &file, file.size());
    const String response = code > 0 && cloudHttp.getSize() <= 8192 ? cloudHttp.getString() : "上传失败";
    endCloudRequest(); file.close(); VoiceInput::recordingStorage().remove("/alarm-upload.wav");
    phoneWeb.send(code > 0 ? code : 502, "application/json; charset=utf-8", response);
  }, []() {
    HTTPUpload &upload = phoneWeb.upload();
    if (upload.status == UPLOAD_FILE_START) {
      alarmMusicUpload.close(); alarmMusicUploadBytes = 0;
      alarmMusicUploadFailed = !WebAdmin::authenticated(phoneWeb) || busy || pendingWebRecording || conversation.active() || AlarmManager::ringing() || !VoiceInput::ready();
      alarmMusicUploadTitle = upload.filename;
      if (alarmMusicUploadTitle.length() > 240) alarmMusicUploadFailed = true;
      if (!alarmMusicUploadFailed) alarmMusicUpload = VoiceInput::recordingStorage().open("/alarm-upload.wav", FILE_WRITE);
      if (!alarmMusicUpload) alarmMusicUploadFailed = true;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      alarmMusicUploadBytes += upload.currentSize;
      if (alarmMusicUploadBytes > 512000) alarmMusicUploadFailed = true;
      if (!alarmMusicUploadFailed && alarmMusicUpload.write(upload.buf, upload.currentSize) != upload.currentSize) alarmMusicUploadFailed = true;
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
      alarmMusicUpload.close();
      if (upload.status == UPLOAD_FILE_ABORTED) alarmMusicUploadFailed = true;
      if (alarmMusicUploadFailed && VoiceInput::ready()) VoiceInput::recordingStorage().remove("/alarm-upload.wav");
    }
  });

  phoneWeb.on("/alarms/add", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const String clockText = phoneWeb.arg("time");
    if (clockText.length() != 5 || clockText[2] != ':') {
      phoneWeb.send(422, "text/html; charset=utf-8",
                    autoReturnPage("闹钟未添加", "时间格式无效。", 5));
      return;
    }
    const int hour = clockText.substring(0, 2).toInt();
    const int minute = clockText.substring(3).toInt();
    const String days = phoneWeb.arg("days");
    const uint8_t mask = days == "workdays" ? 0x3E : days == "weekend" ? 0x41 : 0x7F;
    const bool ok = AlarmManager::addRecurring(hour, minute, mask, phoneWeb.arg("label"));
    RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
    phoneWeb.send(ok ? 200 : 422, "text/html; charset=utf-8",
                  autoReturnPage(ok ? "闹钟已添加" : "闹钟未添加",
                                 ok ? AlarmManager::nextSummary(time(nullptr))
                                    : "时间无效、存储失败或闹钟数量已满。",
                                 5));
  });

  phoneWeb.on("/alarms/timer", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const uint32_t value = static_cast<uint32_t>(phoneWeb.arg("duration").toInt());
    const String unit = phoneWeb.arg("unit");
    const uint32_t seconds = value * (unit == "hours" ? 3600UL : unit == "seconds" ? 1UL : 60UL);
    const bool ok = AlarmManager::addTimer(seconds, "网页计时器", time(nullptr));
    RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
    phoneWeb.send(ok ? 200 : 422, "text/html; charset=utf-8",
                  autoReturnPage(ok ? "计时器已启动" : "计时器未启动",
                                 ok ? AlarmManager::nextSummary(time(nullptr))
                                    : "时长无效、时间未同步、存储失败或数量已满。",
                                 5));
  });

  phoneWeb.on("/alarms/delete", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const bool ok = AlarmManager::remove(static_cast<uint32_t>(phoneWeb.arg("id").toInt()));
    RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
    phoneWeb.send(ok ? 200 : 404, "text/html; charset=utf-8",
                  autoReturnPage(ok ? "已删除" : "未删除",
                                 ok ? "闹钟或计时器已删除。" : "没有找到该编号。", 5));
  });

  phoneWeb.on("/ai/usage", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingWebRecording || conversation.active()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话再读取用量。");
      return;
    }
    const String period = phoneWeb.hasArg("period") ? phoneWeb.arg("period") : "30d";
    if (period != "all" && period != "24h" && period != "7d" && period != "30d") {
      phoneWeb.send(422, "text/plain; charset=utf-8", "统计周期无效。");
      return;
    }
    if (WiFi.status() != WL_CONNECTED || settings.deviceToken.isEmpty() ||
        !beginCloudRequest(requestBaseUrl() + "/voice-api/v1/device-usage?view=compact&period=" + period)) {
      phoneWeb.send(503, "text/plain; charset=utf-8", "服务器连接不可用，请检查网络和设备凭证。");
      return;
    }
    cloudHttp.setTimeout(10000);
    addAuthHeaders(cloudHttp);
    const int code = cloudHttp.GET();
    if (code != HTTP_CODE_OK) {
      endCloudRequest();
      phoneWeb.send(code > 0 ? code : 502, "text/plain; charset=utf-8",
                    "用量请求未成功，HTTP " + String(code));
      return;
    }
    if (cloudHttp.getSize() > 32768) {
      endCloudRequest();
      phoneWeb.send(502, "text/plain; charset=utf-8", "用量记录过大，请在服务器查看。");
      return;
    }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.setContentLength(CONTENT_LENGTH_UNKNOWN);
    phoneWeb.send(200, "application/json; charset=utf-8", "");
    WebAdmin::ResponseStream<WebServer> response(phoneWeb, 32768);
    const int transferred = cloudHttp.writeToStream(&response);
    endCloudRequest(transferred >= 0 && !response.failed());
    if (transferred < 0 || response.failed()) {
      phoneWeb.client().stop();
      return;
    }
    phoneWeb.sendContent("");
  });
  auto dialogueSettingsProxy = []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingWebRecording || conversation.active()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话再读取或保存 AI 设置。");
      return;
    }
    if (WiFi.status() != WL_CONNECTED || settings.deviceToken.isEmpty() ||
        !beginCloudRequest(requestBaseUrl() + "/voice-api/v1/dialogue-settings")) {
      phoneWeb.send(503, "text/plain; charset=utf-8", "服务器连接不可用，请检查网络和设备凭证。");
      return;
    }
    cloudHttp.setTimeout(10000);
    addAuthHeaders(cloudHttp);
    int code;
    if (phoneWeb.method() == HTTP_POST) {
      const String body = phoneWeb.arg("plain");
      if (body.length() > 8000) {
        endCloudRequest();
        phoneWeb.send(413, "text/plain; charset=utf-8", "设置内容过长。");
        return;
      }
      cloudHttp.addHeader("Content-Type", "application/json");
      code = cloudHttp.sendRequest("PUT", body);
    } else {
      code = cloudHttp.GET();
    }
    String response = code == HTTP_CODE_OK ? cloudHttp.getString() : String("");
    endCloudRequest();
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(code == HTTP_CODE_OK ? 200 : (code == 422 ? 422 : 502),
                  code == HTTP_CODE_OK ? "application/json; charset=utf-8" : "text/plain; charset=utf-8",
                  code == HTTP_CODE_OK ? response : "AI 设置请求未成功，HTTP " + String(code));
  };
  phoneWeb.on("/ai/settings", HTTP_GET, dialogueSettingsProxy);
  phoneWeb.on("/ai/settings", HTTP_POST, dialogueSettingsProxy);
  phoneWeb.on("/conversation/clear", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingWebRecording || conversation.active()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话再清空记录。");
      return;
    }
    if (phoneWeb.arg("confirm") != "on") {
      phoneWeb.send(422, "text/plain; charset=utf-8", "请先勾选确认清空。");
      return;
    }
    fs::FS &storage = VoiceInput::recordingStorage();
    if (storage.exists(CONVERSATION_LOG_PATH) && !storage.remove(CONVERSATION_LOG_PATH)) {
      phoneWeb.send(507, "text/plain; charset=utf-8", "本机聊天记录删除失败，未清空服务器记忆。");
      return;
    }
    currentUserText = "";
    currentAssistantText = "";
    historyAssistantText = "";
    currentUserAt = currentAssistantAt = 0;
    RobotDisplay::clearConversation();
    bool cloudOk = true;
    const bool clearCloud = phoneWeb.arg("cloud_memory") == "on";
    if (clearCloud) {
      cloudOk = false;
      if (WiFi.status() == WL_CONNECTED && !settings.deviceToken.isEmpty() &&
          beginCloudRequest(requestBaseUrl() + "/voice-api/v1/memory/clear")) {
        cloudHttp.setTimeout(10000);
        addAuthHeaders(cloudHttp);
        cloudHttp.addHeader("Content-Type", "application/json");
        cloudOk = cloudHttp.POST("{\"confirm\":true}") == HTTP_CODE_OK;
        endCloudRequest();
      }
    }
    phoneWeb.send(cloudOk ? 200 : 502, "text/html; charset=utf-8",
                  autoReturnPage(cloudOk ? "聊天记录已清空" : "本机已清空，服务器记忆未清空",
                                 !cloudOk ? "请确认网络正常、语音任务结束后重试。" :
                                 clearCloud ? "本机记录和本设备的服务器对话记忆已清空。" : "已清空本机记录，服务器对话记忆未改变。", 5));
  });
  phoneWeb.on("/voice/settings", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingWebRecording) { phoneWeb.send(409, "text/plain; charset=utf-8", "请先结束当前对话再保存。"); return; }
    JsonDocument doc;
    VoiceCapture::Settings candidate = VoiceCapture::settings();
    if (deserializeJson(doc, phoneWeb.arg("plain")) ||
        !VoiceCapture::parse(doc.as<JsonVariantConst>(), candidate)) {
      phoneWeb.send(422, "text/plain; charset=utf-8", "收音设置格式或范围无效。");
      return;
    }
    if (!VoiceCapture::save(candidate)) {
      phoneWeb.send(500, "text/plain; charset=utf-8", "收音设置保存失败，请重试。");
      return;
    }
    if (!candidate.continuous && conversation.active()) {
      conversation.cancel();
      OfflineWake::resume();
    }
    traceRuntime("capture_settings_saved", "收音与连续对话设置已保存");
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", statusJson());
  });
  phoneWeb.on("/voice/record/start", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy || pendingWebRecording || pendingDisplayRestartAtMs ||
        pendingOtaCheck || pendingOtaInstall || AlarmManager::ringing()) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在处理任务，请稍后开始收音。");
      return;
    }
    if (!VoiceInput::ready()) {
      phoneWeb.send(503, "text/plain; charset=utf-8", "录音硬件或存储未就绪。");
      return;
    }
    // Return the HTTP response before starting the blocking dialogue pipeline.
    pendingWebRecording = true;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(202, "application/json; charset=utf-8", statusJson());
  });
  phoneWeb.on("/voice/record/finish", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (!manualRecordingActive) {
      phoneWeb.send(409, "text/plain; charset=utf-8", "当前没有网页手动录音。");
      return;
    }
    VoiceInput::requestRecordingFinish();
    manualRecordingActive = false;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", statusJson());
  });
  phoneWeb.on("/voice/stop", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    const bool wasActive = conversation.active();
    pendingWebRecording = false;
    manualRecordingActive = false;
    conversation.cancel();
    RobotControls::requestAbort();
    if (!busy && wasActive) OfflineWake::resume();
    phoneWeb.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  });

  phoneWeb.on("/save", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) { phoneWeb.send(409, "text/plain; charset=utf-8", "设备正在处理语音，请稍后保存。"); return; }
    JsonDocument doc;
    if (phoneWeb.hasArg("server_url")) doc["server_url"] = phoneWeb.arg("server_url");
    if (phoneWeb.hasArg("device_id")) doc["device_id"] = phoneWeb.arg("device_id");
    if (!phoneWeb.arg("device_token").isEmpty()) doc["device_token"] = phoneWeb.arg("device_token");
    if (!phoneWeb.arg("admin_pin").isEmpty()) doc["admin_pin"] = phoneWeb.arg("admin_pin");
    if (phoneWeb.hasArg("chinese_speaker_id")) doc["chinese_speaker_id"] = phoneWeb.arg("chinese_speaker_id").toInt();
    if (phoneWeb.hasArg("english_speaker_id")) doc["english_speaker_id"] = phoneWeb.arg("english_speaker_id").toInt();
    if (phoneWeb.hasArg("speech_speed")) doc["speech_speed"] = phoneWeb.arg("speech_speed").toFloat();
    if (phoneWeb.hasArg("volume_percent")) doc["volume_percent"] = phoneWeb.arg("volume_percent").toInt();
    if (phoneWeb.hasArg("volume_mode")) doc["volume_mode"] = phoneWeb.arg("volume_mode");
    if (phoneWeb.hasArg("offline_wake_enabled")) doc["offline_wake_enabled"] = phoneWeb.arg("offline_wake_enabled") == "on";
    if (phoneWeb.hasArg("offline_wake_model")) doc["offline_wake_model"] = phoneWeb.arg("offline_wake_model");
    if (!phoneWeb.arg("amap_key").isEmpty()) doc["amap_key"] = phoneWeb.arg("amap_key");
    if (phoneWeb.hasArg("weather_city")) doc["weather_city"] = phoneWeb.arg("weather_city");
    if (phoneWeb.hasArg("weather_adcode")) doc["weather_adcode"] = phoneWeb.arg("weather_adcode");
    if (phoneWeb.hasArg("weather_location_mode")) doc["weather_location_mode"] = phoneWeb.arg("weather_location_mode");
    if (phoneWeb.hasArg("weather_latitude")) doc["weather_latitude"] = phoneWeb.arg("weather_latitude").toFloat();
    if (phoneWeb.hasArg("weather_longitude")) doc["weather_longitude"] = phoneWeb.arg("weather_longitude").toFloat();
    if (phoneWeb.hasArg("ota_policy")) doc["ota_auto_update"] = phoneWeb.arg("ota_policy") == "auto";
    doc["connect"] = false;
    String payload;
    serializeJson(doc, payload);
    if (!applyConfig(payload)) {
      phoneWeb.send(422, "text/html; charset=utf-8",
                    autoReturnPage("配置未保存", "配置格式无效，请返回主页检查输入。", 5));
      return;
    }
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8",
                  autoReturnPage("配置已保存",
                                 "设置已保存。修改设备连接码后请重新登录；Wi-Fi 请在联网区域单独保存。",
                                 3));
  });

  phoneWeb.on("/status", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", statusJson());
  });
  phoneWeb.on("/ota/check", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) {
      phoneWeb.send(409, "text/html; charset=utf-8",
                    autoReturnPage("暂时无法检查", "设备正在处理其他任务，请稍后重试。", 5));
      return;
    }
    pendingOtaCheck = true;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(202, "text/html; charset=utf-8",
                  autoReturnPage("已请求检查更新",
                                 "本次只检查和验签，不会下载或安装固件。",
                                 3));
  });
  phoneWeb.on("/ota/install", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (busy) {
      phoneWeb.send(409, "text/html; charset=utf-8",
                    autoReturnPage("暂时无法升级", "设备正在处理其他任务，请稍后重试。", 5));
      return;
    }
    settings.otaDeferredVersion = "";
    saveSettings();
    pendingOtaInstall = true;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(202, "text/html; charset=utf-8",
                  autoReturnPage("已请求安装更新",
                                 "设备将重新检查、验签、下载并安装当前发布版本；成功重启并恢复联网后自动返回主页。",
                                 5));
  });
  phoneWeb.on("/ota/defer", HTTP_POST, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    if (otaAvailableVersion.isEmpty()) {
      phoneWeb.send(409, "text/html; charset=utf-8",
                    autoReturnPage("没有可暂缓版本", "尚未发现可用版本，请先执行只检查更新。", 5));
      return;
    }
    settings.otaDeferredVersion = otaAvailableVersion;
    if (!saveSettings()) {
      phoneWeb.send(500, "text/html; charset=utf-8",
                    autoReturnPage("暂缓失败", "暂缓状态写入 NVS 失败。", 5));
      return;
    }
    otaStatus = "deferred";
    otaMessage = "已暂缓版本 " + otaAvailableVersion;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8",
                  autoReturnPage("已暂不更新",
                                 "该版本已记录到 NVS；以后发现更高版本仍会按更新策略处理。",
                                 3));
  });
  phoneWeb.on("/runtime", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", runtimeJson());
  });
  phoneWeb.on("/conversation.log", HTTP_GET, []() {
    if (!WebAdmin::requireSession(phoneWeb)) return;
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
  auto redirectToPortal = []() {
    phoneWeb.sendHeader("Location", configPortalActive ? configPortalUrl() : "/", true);
    phoneWeb.send(302, "text/plain; charset=utf-8", "正在打开设备配置页面");
  };
  phoneWeb.on("/generate_204", HTTP_ANY, redirectToPortal);
  phoneWeb.on("/gen_204", HTTP_ANY, redirectToPortal);
  phoneWeb.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal);
  phoneWeb.on("/library/test/success.html", HTTP_ANY, redirectToPortal);
  phoneWeb.on("/connecttest.txt", HTTP_ANY, redirectToPortal);
  phoneWeb.on("/ncsi.txt", HTTP_ANY, redirectToPortal);
  phoneWeb.on("/favicon.ico", HTTP_GET, []() { phoneWeb.send(204); });
  phoneWeb.onNotFound([]() {
    if (configPortalActive) {
      phoneWeb.sendHeader("Location", configPortalUrl(), true);
      phoneWeb.send(302, "text/plain; charset=utf-8", "正在打开设备配置页面");
      return;
    }
    phoneWeb.send(404, "text/plain; charset=utf-8", "Not found");
  });
  registerMusicRoutes();
  registerCompanionRoutes();
  registerMicrophoneRoutes();
  phoneWeb.begin();
  webStarted = true;
  const IPAddress pageIp = WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP();
  Serial.println("[设备网页] http://" + pageIp.toString() + "/");
}

void serviceLocalWebFor(uint32_t durationMs) {
  const uint32_t started = millis();
  do {
    serviceFocusTimer();
    if (webStarted) phoneWeb.handleClient();
    delay(5);
  } while (millis() - started < durationMs);
}

bool submitRecording(String &jobId) {
  uint8_t *recording = VoiceInput::recordingData();
  const size_t recordingBytes = VoiceInput::recordingBytes();
  if (!recording || recordingBytes == 0) {
    traceRuntime("error", "内存录音不可用", true);
    return false;
  }
  if (activeServerUrl.isEmpty() && !selectPreferredServer()) return false;

  traceRuntime("uploading", "正在上传 " + String(recordingBytes) + " bytes");
  for (uint8_t attempt = 1; attempt <= 2; ++attempt) {
    if (RobotControls::abortRequested()) return false;
    int code = HTTPC_ERROR_CONNECTION_REFUSED;
    String response;
    cloudHttp.setConnectTimeout(7000);
    cloudHttp.setTimeout(30000);
    if (beginCloudRequest(dialogueBaseUrl() + "/voice-api/v1/dialogue")) {
      addAuthHeaders(cloudHttp);
      cloudHttp.addHeader("Content-Type", "audio/L16");
      cloudHttp.addHeader("X-Device-Context-Version", "1");
      cloudHttp.addHeader("X-Device-Local-Control", "1");
      String weatherContext = weatherAiContext;
      if (weatherContext.isEmpty()) {
        weatherContext = "地点：" + settings.weatherCity +
                         "；天气数据：设备尚未获取成功，请明确告知用户稍后再试。";
      } else {
        const uint32_t ageMinutes = (millis() - weatherAiUpdatedAtMs) / 60000UL;
        weatherContext += "；缓存年龄：" + String(ageMinutes) + "分钟";
        if (ageMinutes >= 90) weatherContext += "；数据状态：可能过期";
      }
      weatherContext = utf8Clip(weatherContext, 512);
      cloudHttp.addHeader("X-Device-Weather", urlEncodeComponent(weatherContext));
      cloudHttp.addHeader("X-Device-Memos",
                          urlEncodeComponent(utf8Clip(MemoManager::aiContext(), 512)));
      code = cloudHttp.sendRequest("POST", recording, recordingBytes);
      if (code == HTTP_CODE_ACCEPTED) response = cloudHttp.getString();
      endCloudRequest();
    }
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
      JsonObjectConst contextAck = doc["context"].as<JsonObjectConst>();
      const bool contextAckValid = !contextAck.isNull() &&
                                   (contextAck["schema"] | 0) >= 1;
      localReportRequired = contextAckValid && (contextAck["local_control"] | false);
      const bool weatherAccepted = contextAckValid &&
                                   (contextAck["weather"] | false);
      if (weatherAccepted) {
        traceRuntime("weather_context_accepted",
                     "云端已接收天气上下文，可用于本轮 AI 回答");
      } else {
        traceRuntime("weather_context_unconfirmed",
                     "云端未确认天气上下文；请更新云端服务后重试");
      }
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

void handleLocalNaturalLanguage(const String &jobId, const String &userText) {
  if (RobotControls::abortRequested() || userText.isEmpty()) return;
  if (lastLocalCommandJobId != jobId) {
    lastLocalCommandJobId = jobId;
    currentLocalResult = ReminderCommands::Result();
    String musicMessage;
    bool musicVolumeChanged = false;
    const bool musicHandled = !AlarmManager::ringing() &&
        !(FocusTimer::active() && userText == "暂停") && FullMusic::handleVoice(userText, musicMessage, &musicVolumeChanged);
    if (musicVolumeChanged) {
      settings.volumePercent = VoiceInput::playbackVolume(); settings.volumePotentiometer = false;
      if (!saveSettings()) musicMessage += " 当前已生效，但未能保存，重启后会恢复原设置。";
    }
    const auto profileResult = musicHandled ? PersonalProfile::CommandResult() : PersonalProfile::handleNaturalLanguage(userText);
    if (profileResult.changed) { profileSyncRequested = true; profileSyncConfirmed = false; nextProfileSyncMs = 0; }
    if (!musicHandled && !profileResult.handled) {
      const auto observed = PersonalProfile::observe(userText);
      if (observed.changed) { profileSyncRequested = true; profileSyncConfirmed = false; nextProfileSyncMs = 0; }
    }
    const auto focusResult = musicHandled || profileResult.handled ? FocusTimer::CommandResult() : FocusTimer::handleNaturalLanguage(userText);
    serviceFocusTimer();
    const RobotPhoto::CommandResult photoResult = musicHandled || profileResult.handled || focusResult.handled ?
        RobotPhoto::CommandResult() : RobotPhoto::handleNaturalLanguage(userText);
    if (musicHandled) {
      currentLocalResult.handled = true; currentLocalResult.message = musicMessage;
      historyAssistantText = musicMessage; traceRuntime("music_command", musicMessage);
    } else if (profileResult.handled || focusResult.handled) {
      currentLocalResult.handled = true;
      currentLocalResult.message = profileResult.handled ? profileResult.message : focusResult.message;
      historyAssistantText = currentLocalResult.message;
    } else if (photoResult.handled) {
      lastPhotoCommandJobId = jobId;
      photoCommandMessage = photoResult.message;
      currentLocalResult.handled = true;
      currentLocalResult.message = photoResult.message;
      if (photoResult.changed) RobotDisplay::setPhotoMode(photoResult.showPhoto);
      traceRuntime("photo_command", photoResult.message, !photoResult.changed);
    } else {
      currentLocalResult = ReminderCommands::execute(userText, time(nullptr));
      if (currentLocalResult.memoChanged) refreshMemoDisplay();
      if (currentLocalResult.alarmChanged) RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
      if (currentLocalResult.handled) {
        traceRuntime("local_command", currentLocalResult.message, false);
        historyAssistantText = currentLocalResult.message;
      }
    }
  }
  if (!localReportRequired || lastLocalReportJobId == jobId) return;
  JsonDocument report;
  report["handled"] = currentLocalResult.handled;
  report["message"] = utf8Clip(currentLocalResult.message, 550);
  if (PersonalProfile::ready()) PersonalProfile::appendSnapshot(report["personal_profile"].to<JsonObject>());
  String body;
  serializeJson(report, body);
  cloudHttp.setConnectTimeout(3000);
  cloudHttp.setTimeout(4000);
  if (!beginCloudRequest(dialogueBaseUrl() + "/voice-api/v1/jobs/" + jobId + "/local-result")) return;
  addAuthHeaders(cloudHttp);
  cloudHttp.addHeader("Content-Type", "application/json");
  const int code = cloudHttp.POST(body);
  JsonDocument ack;
  const bool acknowledged = code == HTTP_CODE_OK && !deserializeJson(ack, cloudHttp.getStream());
  endCloudRequest();
  if (code == HTTP_CODE_OK) {
    lastLocalReportJobId = jobId;
    if (acknowledged && ack["profile_revision"].is<uint32_t>() &&
        ack["profile_revision"].as<uint32_t>() == PersonalProfile::revision()) {
      profileSyncedRevision = PersonalProfile::revision(); profileSyncConfirmed = true;
      profileSyncRequested = false; profileSyncMessage = "云端已同步";
    }
  }
  else traceRuntime("local_result_retry", "设备结果未确认，HTTP " + String(code));
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
  if (!userText.isEmpty()) {
    currentUserText = userText;
    handleLocalNaturalLanguage(jobId, userText);
  }

  const String answerText = assistantTextField(doc.as<JsonVariantConst>());
  if (!answerText.isEmpty()) {
    if (!currentAssistantAt) currentAssistantAt = conversationTimestamp();
    historyAssistantText = answerText;
  }

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
  auto finishPhotoCommand = [&]() {
    setCurrentAssistantText(photoCommandMessage);
    historyAssistantText = photoCommandMessage;
    saveConversationTurn();
    closeCloudConnection();
    // ASR still uses the existing cloud service. Once recognized, the command
    // is applied locally once; do not play an unrelated generated AI response.
    VoiceInput::playTone(1040, 90, 6);
    return true;
  };
  if (!currentUserText.isEmpty()) handleLocalNaturalLanguage(jobId, currentUserText);
  while (millis() - started < JOB_TIMEOUT_MS) {
    if (RobotControls::abortRequested()) {
      closeCloudConnection();
      traceRuntime("cancelled", "中断键已按下，本轮取消");
      RobotDisplay::show(RobotDisplay::Scene::Idle, "已中断", "");
      return false;
    }
    if (webStarted) phoneWeb.handleClient();
    if (RobotControls::abortRequested()) continue;
    if (!currentUserText.isEmpty() && localReportRequired && lastLocalReportJobId != jobId)
      handleLocalNaturalLanguage(jobId, currentUserText);
    if (lastPhotoCommandJobId == jobId) return finishPhotoCommand();
    // /segments is optimized for audio delivery and may not expose ASR text.
    // The Windows reference tool reads /jobs/{id}; do the same at a low rate
    // until the user's recognized sentence becomes available.
    if (currentUserText.isEmpty() &&
        (lastJobTextPollMs == 0 || millis() - lastJobTextPollMs >= 800)) {
      refreshJobConversationText(jobId, false);
      lastJobTextPollMs = millis();
    }
    if (lastPhotoCommandJobId == jobId) return finishPhotoCommand();
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
    if (!polledUserText.isEmpty()) {
      currentUserText = polledUserText;
      handleLocalNaturalLanguage(jobId, polledUserText);
    }
    if (lastPhotoCommandJobId == jobId) return finishPhotoCommand();
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
      RobotDisplay::setConversation(currentUserText, historyAssistantText);
      const uint8_t expression = DialogueExpression::choose(currentUserText, segmentText);
      RobotDisplay::setDialogueExpression(expression);
      if (after < 0 && !FocusTimer::focusing()) RobotInteraction::onReply(expression);
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
      if (lastPhotoCommandJobId == jobId) return finishPhotoCommand();
      RobotDisplay::setConversation(currentUserText, historyAssistantText);
      traceRuntime("completed", "已播放 " + String(after + 1) + " 个分段");
      saveConversationTurn();
      // Conversation text is retained only until the caller returns to the
      // idle page. The idle UI must never show a previous answer.
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

class WakeListeningGuard {
 public:
  WakeListeningGuard() { OfflineWake::suspend(); }
  ~WakeListeningGuard() {
    VoiceInput::clearRecording();
    audioReady = VoiceInput::begin();
    if (!conversation.active()) OfflineWake::resume();
  }
};

void runDialogue(bool pushToTalk = true, bool followUp = false, bool manual = false) {
  if (busy) return;
  FullMusic::Foreground musicGuard;
  if (!musicGuard.ready()) { traceRuntime("music_suspend_failed", "音乐连接未关闭，本轮未开始录音", true); return; }
  RobotInteraction::stop();
  if (!followUp) conversation.start(!manual && VoiceCapture::settings().continuous);
  RobotDisplay::setVoiceSource(!pushToTalk && !manual);
  RobotControls::clearAbortRequest();
  busy = true;
  WakeListeningGuard wakeGuard;
  VoiceInput::setAmbientTracking(false);
  dialogueStartedMs = millis();
  currentUserAt = conversationTimestamp();
  currentAssistantAt = 0;
  lastDialogueError = "";
  currentUserText = "";
  currentAssistantText = "";
  historyAssistantText = "";
  lastHttpCode = 0;
  currentJobId = "";
  currentSegmentIndex = -1;
  lastLocalCommandJobId = "";
  lastLocalReportJobId = "";
  localReportRequired = false;
  currentLocalResult = ReminderCommands::Result();
  lastPhotoCommandJobId = "";
  photoCommandMessage = "";
  traceRuntime("checking_network",
               manual ? "网页手动开始新一轮对话"
                      : pushToTalk ? "说话键开始新一轮对话"
                          : followUp ? "连续对话开始下一轮"
                                     : "离线唤醒开始新一轮对话");
  if (!VoiceInput::ready()) {
    Serial.println(F("[对话] I2S/录音文件系统未就绪。"));
    publishStatus("audio_not_ready");
    traceRuntime("error", "I2S/录音文件系统未就绪", true);
    setState(DeviceState::Error);
    conversation.cancel();
    busy = false;
    return;
  }
  // Startup/configuration already selected and verified activeServerUrl. Avoid
  // opening a throw-away TLS health-check connection immediately before the
  // upload; that pattern can fragment the small internal heap on ESP32-S3.
  if (!connectWifi() || settings.deviceToken.isEmpty() ||
      (activeServerUrl.isEmpty() && !selectPreferredServer())) {
    Serial.println(settings.deviceToken.isEmpty()
                       ? F("[对话] 尚未配置公网设备 Token；请打开设备网页，或长按中断键进入配网。")
                       : F("[对话] 网络或云服务器不可用。"));
    publishStatus("input_or_config_error");
    traceRuntime("error", settings.deviceToken.isEmpty() ? "设备 Token 未配置" : "网络或云服务器不可用", true);
    setState(DeviceState::Error);
    conversation.cancel();
    busy = false;
    return;
  }
  setState(DeviceState::Recording);
  traceRuntime("recording", manual ? "网页收音中，点击结束并发送"
                                  : pushToTalk ? "按住说话键录音，松开发送"
                                        : followUp ? "继续提问，停顿后自动发送"
                                                   : "唤醒成功，停顿后自动发送");
  publishStatus("recording");
  if (manual) {
    Serial.println(F("[网页对话] 正在录音；点击结束并发送，最长 25 秒。"));
  } else if (pushToTalk) {
    Serial.printf("[按键对话] 正在录音；松开 GPIO%d 说话键后发送，最长 20 秒。\n",
                  RobotControls::PIN_TALK_BUTTON);
  } else {
    if (!followUp) VoiceInput::playTone(1040, 90, 6);
    Serial.printf("[离线唤醒] %s 已响应；直接提问，停顿约 %lu ms 后自动发送。\n",
                  OfflineWake::wakeWord(),
                  static_cast<unsigned long>(VoiceCapture::settings().silenceMs));
  }
  VoiceInput::AudioStats stats;
  manualRecordingActive = manual;
  const bool recorded = manual
                            ? VoiceInput::recordPcmManual(
                                  RECORDING_PATH, HANDS_FREE_MAXIMUM_MS, stats, true)
                            : pushToTalk
                            ? VoiceInput::recordPcmPushToTalk(
                                  RECORDING_PATH, RECORD_MINIMUM_MS,
                                  RECORD_MAXIMUM_MS, stats, true)
                            : VoiceInput::recordPcmHandsFree(
                                  RECORDING_PATH, HANDS_FREE_MINIMUM_MS,
                                  HANDS_FREE_MAXIMUM_MS,
                                  VoiceCapture::settings().silenceMs, stats, true);
  manualRecordingActive = false;
  lastRecording = stats;
  if (RobotControls::abortRequested()) {
    traceRuntime("cancelled", "录音已由中断键取消");
    RobotDisplay::show(RobotDisplay::Scene::Idle, "已中断", "");
    setState(DeviceState::Idle);
    conversation.cancel();
    busy = false;
    return;
  }
  if (recorded && !stats.speechDetected) {
    conversation.cancel();
    traceRuntime("no_speech", "未检测到有效说话，结束对话；录音未上传");
    publishStatus("no_speech");
    returnToHomeDisplay();
    busy = false;
    return;
  }
  if (!recorded) {
    Serial.printf("[%s] 录音无效：%s，RMS=%u，Peak=%u\n",
                  manual ? "网页对话" : pushToTalk ? "按键对话" : "离线唤醒", stats.error,
                  stats.rms, stats.peak);
    publishStatus("input_or_config_error");
    traceRuntime("error", "录音无效：" + String(stats.error) + "，RMS=" + String(stats.rms) +
                            ", Peak=" + String(stats.peak) + ", Samples=" + String(stats.samples) +
                            ", Saved=" + String(static_cast<unsigned>(stats.storageBytesWritten)) +
                            ", Errno=" + String(stats.storageError) +
                            ", StorageMs=" + String(stats.storageMs), true);
    setState(DeviceState::Error);
    conversation.cancel();
    busy = false;
    return;
  }
  Serial.printf("[%s] 录音 %lu ms，%u bytes，RMS=%u，Peak=%u，语音=%lu ms，削波=%lu，降噪=%s\n",
                manual ? "网页对话" : pushToTalk ? "按键对话" : "离线唤醒",
                static_cast<unsigned long>(stats.durationMs), static_cast<unsigned>(stats.fileBytes),
                stats.rms, stats.peak, static_cast<unsigned long>(stats.speechMs),
                static_cast<unsigned long>(stats.clippedSamples),
                stats.noiseSuppressionApplied ? "on" : "off");
  traceRuntime("recorded", "PSRAM直传，音频=" + String(stats.samples * 1000U / VoiceInput::SAMPLE_RATE) +
                            "ms，采集=" + String(stats.durationMs) + "ms，准备=" +
                            String(stats.preparationMs) + "ms，写盘=" + String(stats.storageMs) +
                            "ms，网页=" + String(stats.webServiceUs / 1000U) + "ms，DSP=" +
                            String(stats.processingUs / 1000U) + "ms，最大读间隔=" +
                            String(stats.maxReadGapUs / 1000U) + "ms");
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
  const bool submitted = submitRecording(jobId);
  VoiceInput::clearRecording();
  const bool ok = submitted && !RobotControls::abortRequested() && waitAndPlay(jobId);
  const bool cancelled = RobotControls::abortRequested() || dialogueStage == "cancelled";
  publishStatus(ok ? "completed" : cancelled ? "cancelled" : "dialogue_failed");
  if (!ok && !cancelled && dialogueStage != "error") traceRuntime("error", "对话流程失败", true);
  if (cancelled) RobotDisplay::show(RobotDisplay::Scene::Idle, "已中断", "");
  if (ok) returnToHomeDisplay();
  else if (cancelled) returnToHomeDisplay("已中断");
  else setState(DeviceState::Error);
  conversation.complete(ok && !cancelled, millis());
  busy = false;
}

void printStatus() {
  Serial.println(F("\n[云端语音设备状态]"));
  Serial.printf("Firmware: %s (%s)\n", FIRMWARE_VERSION, FirmwareUpdate::HARDWARE_ID);
  Serial.printf("OTA: %s, %s\n", otaStatus.c_str(), otaMessage.c_str());
  Serial.println(statusJson());
  Serial.println(RobotServos::status());
  Serial.printf("I2S: BCLK=%d WS=%d MIC_SD=%d AMP_DIN=%d AMP_SD=%d\n",
                VoiceInput::PIN_I2S_BCLK, VoiceInput::PIN_I2S_WS,
                VoiceInput::PIN_MIC_DATA, VoiceInput::PIN_AMP_DATA, VoiceInput::PIN_AMP_SD);
  Serial.printf("Offline wake: %s, ready=%s, word=%s, model=%s%s%s\n",
                settings.offlineWakeEnabled ? "enabled" : "disabled",
                OfflineWake::ready() ? "yes" : "no",
                OfflineWake::wakeWord(), settings.offlineWakeModel.c_str(),
                strlen(OfflineWake::lastError()) ? ", error=" : "",
                OfflineWake::lastError());
  Serial.printf("Buttons: TALK=%d ABORT=%d; TFT: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BLK=%d; BAT_ADC=%d\n",
                RobotControls::PIN_TALK_BUTTON, RobotControls::PIN_ABORT_BUTTON,
                RobotDisplay::PIN_TFT_SCLK, RobotDisplay::PIN_TFT_MOSI,
                RobotDisplay::PIN_TFT_CS, RobotDisplay::PIN_TFT_DC,
                RobotDisplay::PIN_TFT_RST, RobotDisplay::PIN_TFT_BACKLIGHT,
                RobotDisplay::PIN_BATTERY_ADC);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Device page: http://" + WiFi.localIP().toString() + "/");
  }
  if (configPortalActive) {
    Serial.printf("Config AP: %s, password=%s, page=%s\n", configApSsid.c_str(),
                  CONFIG_AP_PASSWORD, configPortalUrl().c_str());
  }
  Serial.printf("Health: %s\n", activeServerUrl.isEmpty() ? "failed" : "ok");
}

void reportOtaProgress(const String &message) {
  otaMessage = message;
  traceRuntime("ota_update", message);
  publishStatus("ota_update");
}

void performOtaUpdate(bool installAvailable) {
  if (busy) return;
  FullMusic::Foreground musicGuard;
  if (!musicGuard.ready()) { traceRuntime("music_suspend_failed", "音乐连接未关闭，稍后重试更新", true); return; }
  conversation.cancel();
  RobotServos::release();
  Serial.printf("[OTA] 开始前 loop 栈余量：%u 字节。\n",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  busy = true;
  WakeListeningGuard wakeGuard;
  otaStatus = "checking";
  otaMessage = "准备检查线上固件";
  otaAvailableVersion = "";
  setState(DeviceState::Processing);
  traceRuntime("ota_checking", otaMessage);
  publishStatus("ota_checking");
  delay(120);
  RobotDisplay::setSuspended(true);

  closeCloudConnection();
  VoiceInput::releaseAudioHardware();
  audioReady = false;

  FirmwareUpdate::Config config;
  config.manifestUrl = otaManifestUrl();
  config.deviceId = settings.deviceId;
  config.deviceToken = settings.deviceToken;
  config.currentVersion = FIRMWARE_VERSION;
  config.deferredVersion = settings.otaDeferredVersion;
  config.caCertificate = DIGICERT_GLOBAL_ROOT_G2;
  config.installAvailable = installAvailable;
  const FirmwareUpdate::Outcome result = FirmwareUpdate::checkAndInstall(config, reportOtaProgress);
  otaStatus = FirmwareUpdate::resultName(result.result);
  otaMessage = result.message;
  otaAvailableVersion = result.availableVersion;
  if (result.result == FirmwareUpdate::Result::UpdateAvailable &&
      result.availableVersion == settings.otaDeferredVersion) {
    otaStatus = "deferred";
  }
  if (result.result == FirmwareUpdate::Result::Installed) {
    traceRuntime("ota_installed", result.message + "，即将重启");
    publishStatus("ota_installed");
    delay(500);
    ESP.restart();
  }

  audioReady = VoiceInput::begin();
  RobotDisplay::setSuspended(false);
  const bool updateError = result.result != FirmwareUpdate::Result::NoUpdate &&
                           result.result != FirmwareUpdate::Result::UpdateAvailable &&
                           result.result != FirmwareUpdate::Result::NotConfigured;
  traceRuntime(updateError ? "ota_error" : "ota_complete", result.message, updateError);
  if (!updateError && audioReady) {
    returnToHomeDisplay();
  } else {
    if (!audioReady) traceRuntime("audio_error", "OTA 操作后音频硬件恢复失败", true);
    setState(DeviceState::Error);
  }
  busy = false;
  publishStatus(otaStatus);
}

void processConsole() {
  while (Serial.available()) {
    const char value = static_cast<char>(Serial.read());
    if (value == '\n' || value == '\r') {
      serialLine.trim();
      if (serialLine == "/status") printStatus();
      else if (serialLine == "/volume") Serial.println(VoiceInput::volumeDiagnostics());
      else if (serialLine == "/mic" || serialLine.startsWith("/mic ")) {
        if (busy || conversation.active() || pendingWebRecording || CustomWake::trainingPending() || AlarmManager::ringing()) {
          Serial.println(F("[麦克风] 请在当前录音、播放或更新结束后设置。"));
        } else {
          String command = serialLine.length() > 4 ? serialLine.substring(5) : "status";
          command.trim();
          String message;
          FullMusic::Foreground musicGuard;
          if (!musicGuard.ready()) { Serial.println(F("[音乐] 连接尚未关闭，请稍后检测麦克风。")); serialLine = ""; return; }
          OfflineWake::suspend();
          VoiceInput::setAmbientTracking(false);
          busy = true;
          MultiMicConfig::executeCommand(command, message);
          busy = false;
          OfflineWake::resume();
          Serial.println(message);
        }
      }
      else if (serialLine == "/servo" || serialLine.startsWith("/servo ")) {
        String command = serialLine.length() > 6 ? serialLine.substring(7) : "status";
        command.trim();
        String message;
        RobotServos::executeCommand(command, message);
        Serial.println(message);
      }
      else if (serialLine == "/voice") runDialogue();
      else if (serialLine == "/connect") pendingWifiReconnect = true;
      else if (serialLine == "/sync_voice") pendingVoiceSync = true;
      else if (serialLine == "/ota") {
        settings.otaDeferredVersion = "";
        saveSettings();
        pendingOtaInstall = true;
      }
      else if (serialLine == "/ota-check") pendingOtaCheck = true;
      else if (serialLine == "/weather") {
        nextWeatherRefreshAtMs = millis();
        weatherRefreshRequested = true;
        Serial.println(F("[天气] 已安排立即刷新。"));
      }
      else if (serialLine == "/alarms") {
        Serial.println(AlarmManager::toJson(time(nullptr)));
      }
      else if (serialLine.startsWith("/timer ")) {
        const uint32_t seconds = static_cast<uint32_t>(serialLine.substring(7).toInt());
        const bool ok = AlarmManager::addTimer(seconds, "串口计时器", time(nullptr));
        RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
        Serial.println(ok ? F("[闹钟] 计时器已保存。") : F("[闹钟] 计时器保存失败。"));
      }
      else if (serialLine == "/wake list") {
        Serial.println(F("[离线唤醒] 可选模型："));
        for (size_t index = 0; index < OfflineWake::modelOptionCount(); ++index) {
          const OfflineWake::ModelOption &option = OfflineWake::modelOption(index);
          Serial.printf("  %s = %s%s\n", option.id, option.label,
                        settings.offlineWakeModel == option.id ? " [当前]" : "");
        }
      }
      else if (serialLine == "/wake on" || serialLine == "/wake off") {
        settings.offlineWakeEnabled = serialLine.endsWith("on");
        const bool ok = OfflineWake::configure(settings.offlineWakeEnabled,
                                                settings.offlineWakeModel) &&
                        saveSettings();
        Serial.printf("[离线唤醒] %s。\n", ok ? "配置已保存" : OfflineWake::lastError());
      }
      else if (serialLine.startsWith("/wake ")) {
        const String model = serialLine.substring(6);
        if (!OfflineWake::isSupportedModel(model)) {
          Serial.println(F("[离线唤醒] 模型名称无效；输入 /wake list 查看列表。"));
        } else {
          settings.offlineWakeModel = model;
          const bool ok = OfflineWake::configure(settings.offlineWakeEnabled,
                                                  settings.offlineWakeModel) &&
                          saveSettings();
          Serial.printf("[离线唤醒] %s。\n", ok ? "模型已切换并保存" : OfflineWake::lastError());
        }
      }
      else if (serialLine == "/reboot") {
        RobotServos::release();
        ESP.restart();
      }
      else if (serialLine == "/help") {
        Serial.println(F("[麦克风] /mic /mic count 1|2|3 /mic mask 1..7 /mic select auto|1|2|3 /mic test"));
        Serial.println(F("[音量诊断] /volume：查看音量模式、GPIO1原始ADC与滤波ADC。"));
        Serial.println(F("[舵机] /servo /servo 1 90 /servo all 0 0 180 180 /servo home /servo off"));
        Serial.println(F("[命令] /status /config {JSON} /connect /sync_voice /weather /alarms /timer {秒} /wake list|on|off|模型ID /ota-check /ota /voice /reboot"));
      } else if (serialLine.startsWith("/config ")) {
        const String payload = serialLine.substring(8);
        const bool ok = applyConfig(payload);
        Serial.println(ok ? F("[配置] 串口 JSON 已保存，设备将自动重连。")
                          : F("[配置] 串口 JSON 无效，未保存。"));
        publishStatus(ok ? "config_saved" : "invalid_config");
      } else if (!serialLine.isEmpty()) {
        Serial.println(F("[命令] 未知命令；输入 /help 查看帮助。"));
      }
      serialLine = "";
    } else if (serialLine.length() < 512) serialLine += value;
  }
}

void processPendingNetworkActions() {
  if (busy) return;
  bool handledConfigAction = false;
  bool configActionOk = true;
  if (pendingWifiReconnect) {
    handledConfigAction = true;
    pendingWifiReconnect = false;
    const bool connected = connectWifi(true);
    bool serverReady = false;
    if (connected) {
      startPhoneWeb();
      serverReady = selectPreferredServer();
    } else {
      startConfigPortal("目标 Wi-Fi 连接失败");
    }
    configActionOk = configActionOk && connected && serverReady;
    publishStatus(connected ? "connected" : "wifi_failed");
  }
  if (pendingVoiceSync) {
    handledConfigAction = true;
    pendingVoiceSync = false;
    const bool ok = connectWifi() && syncVoiceSettings();
    configActionOk = configActionOk && ok;
    publishStatus(ok ? "voice_synced" : "voice_sync_failed");
  }
  if (handledConfigAction && !pendingOtaInstall && !pendingOtaCheck) {
    if (configActionOk) {
      traceRuntime("idle", "配置已生效，返回待机主页");
      returnToHomeDisplay();
    } else {
      traceRuntime("config_error", "配置已保存，但网络或云端同步失败", true);
      setState(DeviceState::Error);
      if (configPortalActive && WiFi.status() != WL_CONNECTED) {
        RobotDisplay::show(RobotDisplay::Scene::Connecting, "Wi-Fi 配网",
                           configApSsid + "\n" + WiFi.softAPIP().toString());
      }
    }
  }
  if (pendingOtaInstall) {
    pendingOtaInstall = false;
    pendingOtaCheck = false;
    performOtaUpdate(true);
  } else if (pendingOtaCheck) {
    pendingOtaCheck = false;
    performOtaUpdate(false);
  }
}

bool loadAlarmMusic() {
  MusicPlayer::stop();
  if (WiFi.status() != WL_CONNECTED || settings.deviceToken.isEmpty()) return false;
  if (!beginCloudRequest(requestBaseUrl() + "/voice-api/v1/alarm-music/selection")) return false;
  cloudHttp.setTimeout(4000); addAuthHeaders(cloudHttp);
  const int selectionCode = cloudHttp.GET();
  JsonDocument selection;
  const bool selected = selectionCode == 200 && cloudHttp.getSize() > 0 && cloudHttp.getSize() <= 2048 &&
      !deserializeJson(selection, cloudHttp.getString());
  endCloudRequest();
  if (!selected) { traceRuntime("alarm_selection_failed", "HTTP " + String(selectionCode)); return false; }
  const String id = selection["id"] | "";
  if (id.isEmpty() || id.length() > 80 || id.indexOf('/') >= 0) return false;
  if (!beginCloudRequest(requestBaseUrl() + "/voice-api/v1/alarm-music/" + id + "/pcm")) return false;
  addAuthHeaders(cloudHttp); cloudHttp.setTimeout(4000);
  const int code = cloudHttp.GET();
  const int count = cloudHttp.getSize();
  uint8_t *data = code == 200 && count > 0 ? MusicPlayer::prepare(count) : nullptr;
  if (!data) { traceRuntime("alarm_buffer_failed", "HTTP " + String(code) + ", bytes=" + String(count) + ", free_psram=" + String(ESP.getFreePsram())); endCloudRequest(false); return false; }
  Stream *input = cloudHttp.getStreamPtr();
  input->setTimeout(1500);
  const size_t received = AudioDownload::readBounded(*input, data, static_cast<size_t>(count),
      10000, []() { return RobotControls::abortRequested(); });
  endCloudRequest(received == static_cast<size_t>(count));
  if (received != static_cast<size_t>(count)) { traceRuntime("alarm_download_failed", String(received) + "/" + String(count)); MusicPlayer::stop(); return false; }
  MusicPlayer::loaded(selection["title"] | id);
  return true;
}

bool processAlarmClock() {
  const time_t now = time(nullptr);
  AlarmManager::Entry fired;
  if (!busy && AlarmManager::poll(now, fired)) {
    if (alarmPreview) { alarmPreview = false; MusicPlayer::stop(); FullMusic::release(); }
    alarmMusicHeld = FullMusic::hold();
    if (!alarmMusicHeld) { FullMusic::pause(); traceRuntime("music_suspend_failed", "音乐连接未关闭，本次延后响铃", true); return true; }
    alarmRingStartedAtMs = millis();
    lastAlarmToneAtMs = 0;
    alarmTalkWasPressed = RobotControls::talkPressed();
    RobotDisplay::show(RobotDisplay::Scene::Alarm, "闹钟提醒", fired.label);
    traceRuntime("alarm_ringing", alarmEntryText(fired));
    VoiceInput::setAmbientTracking(false);
    if (conversation.active()) { conversation.cancel(); OfflineWake::resume(); }
    busy = true;
    const bool loaded = loadAlarmMusic();
    busy = false;
    traceRuntime(loaded ? "alarm_music_ready" : "alarm_music_fallback", MusicPlayer::status());
  }
  if (!AlarmManager::ringing()) {
    if (alarmMusicHeld) { alarmMusicHeld = false; FullMusic::release(); }
    if (!alarmPreview && MusicPlayer::available()) { MusicPlayer::stop(); returnToHomeDisplay(); }
    alarmTalkWasPressed = RobotControls::talkPressed();
    return false;
  }
  if (!alarmMusicHeld) { alarmMusicHeld = FullMusic::hold(); if (!alarmMusicHeld) return true; }

  const bool talkPressed = RobotControls::talkPressed();
  if (!busy && OfflineWake::consumeDetected()) {
    MusicPlayer::pause();
    runDialogue(false);
    // Alarm control is one turn; unrelated questions resume the same clip.
    conversation.cancel(); OfflineWake::resume();
    if (!AlarmManager::ringing()) { MusicPlayer::stop(); returnToHomeDisplay(); }
    else RobotDisplay::show(RobotDisplay::Scene::Alarm, "闹钟提醒", AlarmManager::ringingEntry().label);
    return true;
  }
  if (RobotControls::abortRequested()) {
    RobotControls::consumeAbortRequest();
    AlarmManager::dismiss();
    MusicPlayer::stop();
    traceRuntime("alarm_dismissed", "中断键已关闭提醒");
    returnToHomeDisplay();
    return true;
  }
  if (talkPressed && !alarmTalkWasPressed) {
    const bool snoozed = AlarmManager::snooze(now, 5UL * 60UL);
    traceRuntime(snoozed ? "alarm_snoozed" : "alarm_dismissed",
                 snoozed ? "已稍后5分钟提醒" : "稍后提醒保存失败，继续响铃");
    if (snoozed) { MusicPlayer::stop(); returnToHomeDisplay(); }
    alarmTalkWasPressed = true;
    return true;
  }
  alarmTalkWasPressed = talkPressed;
  if (alarmRingStartedAtMs != 0 && millis() - alarmRingStartedAtMs >= 10UL * 60UL * 1000UL) {
    AlarmManager::dismiss();
    MusicPlayer::stop();
    traceRuntime("alarm_dismissed", "响铃10分钟后自动关闭");
    returnToHomeDisplay();
    return true;
  }
  if (MusicPlayer::available()) { MusicPlayer::tick(); }
  else if (lastAlarmToneAtMs == 0 || millis() - lastAlarmToneAtMs >= 1600) {
    lastAlarmToneAtMs = millis();
    const uint16_t frequency = ((millis() / 1600) & 1U) ? 1040 : 880;
    VoiceInput::playTone(frequency, 360, 18);
  }
  return true;
}

void scheduleNightlyOta() {
  const uint32_t nowMs = millis();
  if (!clockReady || WiFi.status() != WL_CONNECTED || busy ||
      (lastOtaSchedulePollAtMs != 0 && nowMs - lastOtaSchedulePollAtMs < OTA_SCHEDULE_POLL_MS)) {
    return;
  }
  lastOtaSchedulePollAtMs = nowMs;
  const time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm local {};
  localtime_r(&now, &local);
  const int dateKey = (local.tm_year + 1900) * 400 + local.tm_yday;
  if (local.tm_hour != OTA_NIGHT_HOUR || local.tm_min >= OTA_NIGHT_WINDOW_MINUTES ||
      dateKey == lastOtaScheduleDateKey) {
    return;
  }
  lastOtaScheduleDateKey = dateKey;
  if (settings.otaAutoUpdate) pendingOtaInstall = true;
  else pendingOtaCheck = true;
  Serial.printf("[OTA] 凌晨计划已触发：%s。\n",
                settings.otaAutoUpdate ? "检查并安装" : "仅检查");
}

void serviceConfigButton() {
  if (busy) {
    abortHeldSinceMs = 0;
    abortLongPressHandled = false;
    return;
  }
  const bool pressed = digitalRead(RobotControls::PIN_ABORT_BUTTON) == LOW;
  if (!pressed) {
    abortHeldSinceMs = 0;
    abortLongPressHandled = false;
    return;
  }
  if (abortHeldSinceMs == 0) abortHeldSinceMs = millis();
  if (!abortLongPressHandled && millis() - abortHeldSinceMs >= CONFIG_BUTTON_HOLD_MS) {
    abortLongPressHandled = true;
    RobotControls::clearAbortRequest();
    startConfigPortal("长按中断键进入配网");
  }
}

void serviceConfigPortal() {
  const uint32_t now = millis();
  static uint32_t lastSetupRefreshMs = 0;
  if (configPortalActive && now - lastSetupRefreshMs >= 500U) {
    updateProvisioningDisplay();
    lastSetupRefreshMs = now;
  }
  if (configPortalActive) configDns.processNextRequest();

  if (WiFi.status() == WL_CONNECTED) {
    wifiDisconnectedSinceMs = 0;
    if (configPortalActive && configPortalCloseAtMs != 0 &&
        static_cast<int32_t>(now - configPortalCloseAtMs) >= 0) {
      stopConfigPortal();
      returnToHomeDisplay();
    }
    return;
  }

  configPortalCloseAtMs = 0;
  if (!configPortalActive) {
    if (wifiDisconnectedSinceMs == 0) wifiDisconnectedSinceMs = now;
    if (now - wifiDisconnectedSinceMs >= WIFI_LOST_PORTAL_DELAY_MS) {
      startConfigPortal("Wi-Fi 已断开超过 30 秒");
    }
    return;
  }

  if (!busy && !pendingWifiReconnect && WifiProfiles::count() > 0 &&
      static_cast<int32_t>(now - nextConfigWifiRetryAtMs) >= 0) {
    nextConfigWifiRetryAtMs = now + CONFIG_PORTAL_RETRY_MS;
    pendingWifiReconnect = true;
    Serial.println(F("[配网] 定时重新扫描，只连接附近的已保存 Wi-Fi。"));
  }
}

}  // namespace CloudDevice

void setup() {
  using namespace CloudDevice;
  Serial.begin(SERIAL_BAUD);
  delay(350);
  RobotControls::begin();
  if (!RobotServos::begin()) {
    Serial.println(F("[舵机] 四路 SG90 初始化失败，其他功能继续启动。"));
  }
  const bool displayReady = RobotDisplay::begin();
  if (!displayReady) Serial.println(F("[显示] 显示任务初始化失败，语音功能继续启动。"));
  if (!AlarmManager::begin()) {
    Serial.println(F("[闹钟] NVS 数据读取失败；本次启动使用空列表。"));
  }
  RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
  if (!initializeWeatherWorker()) {
    Serial.println(F("[天气] 常驻任务初始化失败；稍后将自动重试。"));
  }
  traceRuntime("booting", "固件启动");
  setState(DeviceState::Booting);
  loadSettings();
  if (!VoiceCapture::begin()) {
    Serial.println(F("[收音] 设置读取失败，本次使用默认值。"));
  }
  if (!MultiMicConfig::begin()) {
    Serial.println(F("[麦克风] 配置读取失败，本次默认仅 MIC1。"));
  }
  if (!RobotInteraction::begin()) Serial.println(F("[互动动作] 设置读取失败，当前保持关闭。"));
  if (!FocusTimer::begin()) Serial.println(F("[番茄钟] 状态读取失败，请查看番茄钟页。"));
  VoiceInput::setServiceCallback([]() {
    serviceFocusTimer();
    static uint32_t lastServiceAt = 0;
    if (webStarted && millis() - lastServiceAt >= 20U) {
      phoneWeb.handleClient();
      lastServiceAt = millis();
    }
  });
  audioReady = VoiceInput::begin();
  if (!audioReady) {
    Serial.println(F("[启动] I2S/录音文件系统初始化失败。Wi-Fi 配置热点仍可使用。"));
    setState(DeviceState::Error);
  } else if (!MemoManager::begin(VoiceInput::recordingStorage())) {
    Serial.println(F("[便签] 存储初始化失败；语音和其他功能继续运行。"));
  }
  if (audioReady) {
    if (!PersonalProfile::begin(VoiceInput::recordingStorage()))
      Serial.println(F("[个人记忆] 存储读取失败，未覆盖原数据。"));
    if (!CustomWake::begin(VoiceInput::recordingStorage()))
      Serial.println(F("[自定义唤醒] 存储读取失败，实验唤醒未启用。"));
    if (!RobotPhoto::begin(VoiceInput::recordingStorage(), RobotDisplay::width(),
                          RobotDisplay::height())) {
      Serial.println(F("[照片] 图片存储初始化失败；其他功能继续运行。"));
    }
  }
  const bool offlineWakeReady =
      audioReady && OfflineWake::begin(settings.offlineWakeEnabled,
                                       settings.offlineWakeModel);
  if (settings.offlineWakeEnabled && !offlineWakeReady) {
    Serial.printf("[离线唤醒] 初始化失败：%s。说话键和云端对话仍可使用。\n",
                  OfflineWake::lastError());
    traceRuntime("wake_error", OfflineWake::lastError(), true);
  }
  refreshMemoDisplay();
  previousTalkPressed = RobotControls::talkPressed();
  if (connectWifi()) {
    startPhoneWeb();
    selectPreferredServer();
  } else {
    startConfigPortal(settings.wifiSsid.isEmpty() ? "尚未配置 Wi-Fi" : "Wi-Fi 连接失败");
  }
  closeCloudConnection();
  if (!FullMusic::begin(baseUrlWithScheme(settings.serverUrl, true), settings.deviceId, settings.deviceToken, DIGICERT_GLOBAL_ROOT_G2))
    traceRuntime("music_init_failed", "完整音乐缓存或任务初始化失败", true);
  publishStatus("ready");
  setState(audioReady ? DeviceState::Idle : DeviceState::Error);
  if (audioReady) {
    if (!FirmwareUpdate::confirmRunningImage()) {
      traceRuntime("ota_confirm_error", "无法确认当前 OTA 镜像", true);
    }
  } else {
    traceRuntime("ota_unconfirmed", "音频自检失败，不确认新 OTA 镜像", true);
  }
  nextWeatherRefreshAtMs = millis() + WEATHER_FIRST_REFRESH_DELAY_MS;
  Serial.println(F("\nESP32-S3 云端语音设备已启动。"));
  Serial.printf("固件版本：%s；硬件通道：%s。\n", FIRMWARE_VERSION, FirmwareUpdate::HARDWARE_ID);
  Serial.println(VoiceInput::microphoneDiagnostics());
  Serial.printf("说话键 GPIO%d：按住录音、松开发送；中断键 GPIO%d：短按中断，长按 5 秒进入配网。\n",
                RobotControls::PIN_TALK_BUTTON, RobotControls::PIN_ABORT_BUTTON);
  Serial.printf("离线唤醒：%s；当前名字：%s。\n",
                OfflineWake::ready() && settings.offlineWakeEnabled ? "已开启"
                                                                    : "未开启",
                OfflineWake::wakeWord());
  Serial.println(F("串口输入 /help 查看配置和维护命令。"));
  printStatus();
  if (audioReady && !configPortalActive) traceRuntime("idle", "设备就绪");
}

void loop() {
  using namespace CloudDevice;
  FullMusic::configure(baseUrlWithScheme(settings.serverUrl, true), settings.deviceId, settings.deviceToken);
  VoiceInput::setAmbientTracking(!busy && !conversation.active() && !AlarmManager::ringing() && !FullMusic::playing());
  static uint32_t lastAmbientPoll = 0;
  if (!busy && !conversation.active() && !AlarmManager::ringing() && !FullMusic::playing() && !OfflineWake::listening() &&
      millis() - lastAmbientPoll >= 40) {
    int16_t samples[160]; lastAmbientPoll = millis();
    VoiceInput::readMicrophoneMono16(samples, 160, 20);
  }
  if (webStarted) phoneWeb.handleClient();
  if (pendingDisplayRestartAtMs != 0 &&
      static_cast<int32_t>(millis() - pendingDisplayRestartAtMs) >= 0) {
    pendingDisplayRestartAtMs = 0;
    Serial.println(F("[显示] 屏幕型号已变更，正在重启应用新配置。"));
    RobotServos::release();
    delay(80);
    ESP.restart();
  }
  WifiScan::poll();
  serviceConfigPortal();
  processConsole();
  serviceFocusTimer();
  scheduleNightlyOta();
  processPendingNetworkActions();
  if (!busy && !(configPortalActive && WiFi.status() != WL_CONNECTED) &&
      displayReturnAtMs != 0 &&
      static_cast<int32_t>(millis() - displayReturnAtMs) >= 0) {
    displayReturnAtMs = 0;
    traceRuntime("idle", "状态提示结束，返回待机主页");
    returnToHomeDisplay();
  }
  WeatherResult weatherResult{};
  if (weatherRefreshRunning && weatherResultQueue &&
      xQueueReceive(weatherResultQueue, &weatherResult, 0) == pdTRUE) {
    weatherRefreshRunning = false;
    if (weatherResult.success && weatherResult.aiContext[0] != '\0') {
      weatherAiContext = String(weatherResult.aiContext);
      weatherAiUpdatedAtMs = millis();
      traceRuntime("weather_context_ready", "天气已同步到 AI 对话上下文");
    }
    if (weatherResult.locationResolved &&
        settings.weatherCity == String(weatherResult.query) &&
        (settings.weatherLocationMode == "ip") == weatherResult.locateByIp) {
      const bool locationChanged =
          settings.weatherCity != String(weatherResult.city) ||
          settings.weatherAdcode != String(weatherResult.adcode) ||
          settings.weatherLatitude != weatherResult.latitude ||
          settings.weatherLongitude != weatherResult.longitude;
      settings.weatherCity = String(weatherResult.city);
      settings.weatherAdcode = String(weatherResult.adcode);
      settings.weatherLatitude = weatherResult.latitude;
      settings.weatherLongitude = weatherResult.longitude;
      weatherLocationNeedsResolution = false;
      if (locationChanged) {
        if (saveSettings()) {
          Serial.println(F("[天气] 高德地点、adcode 和坐标已保存。"));
        } else {
          Serial.println(F("[天气] 新地点已生效，但写入 NVS 失败。"));
        }
      }
    }
    nextWeatherRefreshAtMs = weatherRefreshRequested
                                 ? millis()
                                 : millis() + (weatherResult.success
                                                   ? WEATHER_REFRESH_INTERVAL_MS
                                                   : WEATHER_RETRY_INTERVAL_MS);
  }
  if (!busy && !weatherRefreshRunning && WiFi.status() == WL_CONNECTED &&
      !FullMusic::playing() &&
      static_cast<int32_t>(millis() - nextWeatherRefreshAtMs) >= 0) {
    weatherRefreshRequested = false;
    if (!startWeatherRefresh()) {
      nextWeatherRefreshAtMs = millis() + WEATHER_RETRY_INTERVAL_MS;
    }
  }
  if (millis() - lastDisplayNetworkAtMs >= 5000 || lastDisplayNetworkAtMs == 0) {
    RobotDisplay::setNetwork(WiFi.status() == WL_CONNECTED,
                             WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : -127,
                             WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                 : configPortalActive ? WiFi.softAPIP().toString() : "");
    RobotDisplay::setAlarmStatus(AlarmManager::nextSummary(time(nullptr)));
    lastDisplayNetworkAtMs = millis();
  }
  if (processAlarmClock()) {
    if (conversation.active()) {
      conversation.cancel();
      OfflineWake::resume();
    }
    previousTalkPressed = RobotControls::talkPressed();
    delay(8);
    return;
  }
  if (!busy && !conversation.active() && !pendingWebRecording && CustomWake::trainingPending()) {
    FullMusic::Foreground musicGuard;
    if (!musicGuard.ready()) return;
    RobotInteraction::stop();
    busy = true;
    VoiceInput::setAmbientTracking(false);
    CustomWake::processQueuedTraining([]() {
      serviceFocusTimer();
      if (RobotControls::abortRequested()) CustomWake::cancelTraining();
      if (webStarted) phoneWeb.handleClient();
    });
    busy = false;
    returnToHomeDisplay();
    return;
  }
  if (!busy && !conversation.active() && !pendingWebRecording && !pendingFocusAnnouncement.isEmpty()) {
    FullMusic::Foreground musicGuard;
    if (!musicGuard.ready()) return;
    const String announcement = pendingFocusAnnouncement;
    pendingFocusAnnouncement = "";
    OfflineWake::suspend();
    VoiceInput::playTone(FocusTimer::focusing() ? 880 : 660, 180, 8);
    traceRuntime("focus_phase", announcement);
    OfflineWake::resume();
  }
  if (!busy && !conversation.active() && !pendingWebRecording && PersonalProfile::ready() &&
      !RobotControls::talkPressed() && !RobotControls::abortRequested() &&
      WiFi.status() == WL_CONNECTED && !settings.deviceToken.isEmpty() &&
      (profileSyncRequested || !profileSyncConfirmed || profileSyncedRevision != PersonalProfile::revision()) &&
      (nextProfileSyncMs == 0 || static_cast<int32_t>(millis() - nextProfileSyncMs) >= 0)) {
    nextProfileSyncMs = millis() + 60000U;
    syncPersonalProfile();
  }
  serviceConfigButton();
  const bool talkPressed = RobotControls::talkPressed();
  if (!busy && RobotControls::abortRequested()) {
    RobotInteraction::stop();
    const bool wasActive = conversation.active();
    pendingWebRecording = false;
    conversation.cancel();
    RobotControls::consumeAbortRequest();
    if (wasActive) OfflineWake::resume();
    else if (alarmPreview) { alarmPreview = false; MusicPlayer::stop(); FullMusic::release(); }
    else FullMusic::stop();
  } else if (!busy && pendingWebRecording) {
    pendingWebRecording = false;
    runDialogue(false, false, true);
  } else if (!busy && !previousTalkPressed && talkPressed) {
    delay(28);
    if (RobotControls::talkPressed()) runDialogue();
  } else if (!busy && conversation.takeNext(millis())) {
    runDialogue(false, true);
  } else if (!busy && !conversation.active() && OfflineWake::consumeDetected()) {
    runDialogue(false);
  }
  previousTalkPressed = RobotControls::talkPressed();
  if (!busy && RobotControls::abortRequested()) RobotControls::consumeAbortRequest();
  if (!busy && !conversation.active()) {
    if (alarmPreview) {
      if (millis() - alarmPreviewStarted >= 15000 || RobotControls::abortRequested()) {
        alarmPreview = false; MusicPlayer::stop(); FullMusic::release();
      } else MusicPlayer::tick();
    } else FullMusic::tick();
  }
  delay(FullMusic::playing() ? 1 : 8);
}
