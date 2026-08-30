#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <FS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "voice_input.h"

namespace LanDevice {

constexpr uint32_t SERIAL_BAUD = 460800;
constexpr uint32_t WIFI_TIMEOUT_MS = 20000;
constexpr uint32_t RECORD_MINIMUM_MS = 400;
constexpr uint32_t RECORD_MAXIMUM_MS = 20000;
constexpr uint16_t DEFAULT_HTTP_PORT = 8765;
constexpr uint16_t DEFAULT_DISCOVERY_PORT = 8764;
constexpr char DISCOVERY_REQUEST[] = "ESP32_AI_DISCOVER_V1";
constexpr char DISCOVERY_REPLY[] = "ESP32_AI_SERVER_V1 ";
constexpr char RECORDING_PATH[] = "/lan_question.pcm";
constexpr size_t MAX_CONSOLE_LINE = 768;
constexpr size_t MAX_BLE_JSON = 768;
constexpr char BLE_SERVICE_UUID[] = "6f7a0001-7f9e-4a0b-9a8b-51f60f6d1000";
constexpr char BLE_CONFIG_UUID[] = "6f7a0002-7f9e-4a0b-9a8b-51f60f6d1000";
constexpr char BLE_COMMAND_UUID[] = "6f7a0003-7f9e-4a0b-9a8b-51f60f6d1000";
constexpr char BLE_STATUS_UUID[] = "6f7a0004-7f9e-4a0b-9a8b-51f60f6d1000";

enum class DeviceState : uint8_t { Booting, Idle, Recording, Processing, Playing, Error };

struct Settings {
  String wifiSsid;
  String wifiPassword;
  String serverUrl;
  String deviceToken;
  uint16_t discoveryPort = DEFAULT_DISCOVERY_PORT;
  uint8_t volumePercent = 50;
  bool volumePotentiometer = false;
};

enum class BleRequestType : uint8_t { Config, Command };

struct BleRequest {
  BleRequestType type;
  char payload[MAX_BLE_JSON + 1];
};

Settings settings;
Preferences preferences;
String serialLine;
bool previousButton = HIGH;
bool busy = false;
bool webStarted = false;
WebServer phoneWeb(80);
QueueHandle_t bleQueue = nullptr;
BLECharacteristic *bleConfigCharacteristic = nullptr;
BLECharacteristic *bleStatusCharacteristic = nullptr;
bool bleClientConnected = false;
uint32_t blePin = 0;
String bleDeviceName;
wl_status_t previousWifiStatus = WL_IDLE_STATUS;
uint32_t lastBleStatusMs = 0;

String deviceId() {
  return "esp32-s3-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);
}

String htmlEscape(String value) {
  value.replace("&", "&amp;");
  value.replace("\"", "&quot;");
  value.replace("<", "&lt;");
  value.replace(">", "&gt;");
  return value;
}

String urlEncode(const String &value) {
  constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += HEX_DIGITS[c >> 4];
      encoded += HEX_DIGITS[c & 0x0F];
    }
  }
  return encoded;
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

String normalizedServerUrl(String value) {
  value.trim();
  while (value.endsWith("/")) value.remove(value.length() - 1);
  return value;
}

void loadSettings() {
  preferences.begin("ai-config", true);
  settings.wifiSsid = preferences.getString("wifi_ssid", "");
  settings.wifiPassword = preferences.getString("wifi_pass", "");
  settings.serverUrl = preferences.getString("lan_server", "");
  settings.deviceToken = preferences.getString("lan_token", "");
  settings.discoveryPort = preferences.getUShort("lan_discovery", DEFAULT_DISCOVERY_PORT);
  settings.volumePercent = preferences.getUChar("volume", 50);
  settings.volumePotentiometer = preferences.getBool("volume_pot", false);
  preferences.end();
  settings.serverUrl = normalizedServerUrl(settings.serverUrl);
  settings.volumePercent = constrain(settings.volumePercent, 0, 100);
  VoiceInput::setPlaybackVolume(settings.volumePercent);
  VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
}

bool saveLanSettings() {
  preferences.begin("ai-config", false);
  bool ok = true;
  ok &= preferences.putString("lan_server", settings.serverUrl) > 0;
  ok &= preferences.putString("lan_token", settings.deviceToken) > 0 || settings.deviceToken.isEmpty();
  ok &= preferences.putUShort("lan_discovery", settings.discoveryPort) > 0;
  ok &= preferences.putUChar("volume", settings.volumePercent) > 0;
  ok &= preferences.putBool("volume_pot", settings.volumePotentiometer) > 0;
  preferences.end();
  return ok;
}

bool saveProvisioningSettings() {
  preferences.begin("ai-config", false);
  bool ok = true;
  ok &= preferences.putString("wifi_ssid", settings.wifiSsid) > 0 || settings.wifiSsid.isEmpty();
  ok &= preferences.putString("wifi_pass", settings.wifiPassword) > 0 || settings.wifiPassword.isEmpty();
  ok &= preferences.putString("lan_server", settings.serverUrl) > 0 || settings.serverUrl.isEmpty();
  ok &= preferences.putUShort("lan_discovery", settings.discoveryPort) > 0;
  ok &= preferences.putUChar("volume", settings.volumePercent) > 0;
  ok &= preferences.putBool("volume_pot", settings.volumePotentiometer) > 0;
  preferences.end();
  return ok;
}

bool connectWifi(bool force = false) {
  if (settings.wifiSsid.isEmpty()) {
    Serial.println(F("[Wi-Fi] NVS 中没有 Wi-Fi。请先烧回稳定版并通过 BLE/控制台配置一次。"));
    return false;
  }
  if (WiFi.status() == WL_CONNECTED && !force) return true;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  if (force) {
    WiFi.disconnect(false, false);
    delay(120);
  }
  Serial.printf("[Wi-Fi] 正在连接 %s", settings.wifiSsid.c_str());
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPassword.c_str());
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WIFI_TIMEOUT_MS) {
    delay(350);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[Wi-Fi] 连接失败。"));
    return false;
  }
  Serial.printf("[Wi-Fi] 已连接，IP=%s，RSSI=%d dBm\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

bool discoverServer(bool persist = true) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiUDP udp;
  if (!udp.begin(0)) {
    Serial.println(F("[局域网] 无法启动 UDP 自动发现。"));
    return false;
  }

  IPAddress broadcast(255, 255, 255, 255);
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    udp.beginPacket(broadcast, settings.discoveryPort);
    udp.write(reinterpret_cast<const uint8_t *>(DISCOVERY_REQUEST), strlen(DISCOVERY_REQUEST));
    udp.endPacket();
    const uint32_t deadline = millis() + 1200;
    while (static_cast<int32_t>(deadline - millis()) > 0) {
      const int packetSize = udp.parsePacket();
      if (packetSize > 0) {
        char reply[96] = {};
        const int count = udp.read(reply, sizeof(reply) - 1);
        if (count > 0 && String(reply).startsWith(DISCOVERY_REPLY)) {
          const uint16_t port = static_cast<uint16_t>(String(reply + strlen(DISCOVERY_REPLY)).toInt());
          if (port > 0) {
            settings.serverUrl = "http://" + udp.remoteIP().toString() + ":" + String(port);
            udp.stop();
            if (persist) saveLanSettings();
            Serial.println("[局域网] 自动发现服务端：" + settings.serverUrl);
            return true;
          }
        }
      }
      delay(20);
    }
  }
  udp.stop();
  Serial.println(F("[局域网] 未发现电脑服务端，将尝试已保存地址。"));
  return false;
}

void addDeviceHeaders(HTTPClient &http) {
  http.addHeader("X-Device-ID", deviceId());
  if (!settings.deviceToken.isEmpty()) http.addHeader("X-Device-Token", settings.deviceToken);
}

void startPhoneWeb() {
  if (webStarted || WiFi.status() != WL_CONNECTED) return;
  phoneWeb.on("/", HTTP_GET, []() {
    String page;
    page.reserve(1200);
    page = F("<!doctype html><html lang='zh-CN'><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>ESP32 AI</title><style>html,body,iframe{margin:0;width:100%;height:100%;"
             "border:0}body{font-family:system-ui;background:#123a63;color:white}.msg{padding:24px}"
             "</style></head><body>");
    if (settings.serverUrl.isEmpty()) {
      page += F("<div class='msg'><h2>尚未发现电脑服务端</h2>"
                "<p>请先在电脑运行 start_server.cmd，然后刷新本页。</p></div>");
    } else {
      const String source = htmlEscape(settings.serverUrl) + "/device/" + deviceId() +
                            "#token=" + urlEncode(settings.deviceToken);
      page += "<iframe title='ESP32 AI 对话记录' src=\"" + source + "\"></iframe>";
    }
    page += F("</body></html>");
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "text/html; charset=utf-8", page);
  });
  phoneWeb.on("/health", HTTP_GET, []() {
    phoneWeb.send(200, "application/json; charset=utf-8",
                  "{\"ok\":true,\"device_id\":\"" + deviceId() + "\"}");
  });
  phoneWeb.on("/info", HTTP_GET, []() {
    JsonDocument doc;
    doc["device_id"] = deviceId();
    doc["ip"] = WiFi.localIP().toString();
    doc["ssid"] = settings.wifiSsid;
    doc["server_url"] = settings.serverUrl;
    doc["ble_name"] = bleDeviceName;
    doc["ble_pairing_pin"] = blePin;
    String body;
    serializeJson(doc, body);
    phoneWeb.sendHeader("Cache-Control", "no-store");
    phoneWeb.send(200, "application/json; charset=utf-8", body);
  });
  phoneWeb.on("/favicon.ico", HTTP_GET, []() { phoneWeb.send(204); });
  phoneWeb.onNotFound([]() { phoneWeb.send(404, "text/plain; charset=utf-8", "Not found"); });
  phoneWeb.begin();
  webStarted = true;
  Serial.println("[手机网页] http://" + WiFi.localIP().toString() + "/");
}

bool serverHealthCheck() {
  if (settings.serverUrl.isEmpty()) return false;
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(settings.serverUrl + "/health")) return false;
  const int code = http.GET();
  http.end();
  return code == HTTP_CODE_OK;
}

bool ensureServer() {
  if (serverHealthCheck()) return true;
  if (discoverServer(true) && serverHealthCheck()) return true;
  Serial.println(F("[局域网] 服务端不可用。请在电脑运行 start_server.cmd，并允许防火墙访问专用网络。"));
  return false;
}

String buildBleConfigJson() {
  JsonDocument doc;
  doc["ssid"] = settings.wifiSsid;
  doc["wifi_password_configured"] = !settings.wifiPassword.isEmpty();
  doc["server_url"] = settings.serverUrl;
  doc["discovery_port"] = settings.discoveryPort;
  doc["volume_percent"] = settings.volumePercent;
  doc["volume_mode"] = settings.volumePotentiometer ? "pot" : "fixed";
  String output;
  serializeJson(doc, output);
  return output;
}

String buildBleStatusJson(const String &message = "") {
  JsonDocument doc;
  doc["wifi"] = WiFi.status() == WL_CONNECTED ? "connected" : "disconnected";
  doc["ssid"] = settings.wifiSsid;
  if (WiFi.status() == WL_CONNECTED) {
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["phone_url"] = "http://" + WiFi.localIP().toString() + "/";
  }
  doc["server_url"] = settings.serverUrl;
  doc["discovery_port"] = settings.discoveryPort;
  doc["ble_name"] = bleDeviceName;
  doc["heap_free"] = ESP.getFreeHeap();
  if (!message.isEmpty()) doc["message"] = message;
  String output;
  serializeJson(doc, output);
  return output;
}

void publishBleStatus(const String &message = "") {
  if (!bleStatusCharacteristic) return;
  const String status = buildBleStatusJson(message);
  bleStatusCharacteristic->setValue(status);
  if (bleClientConnected) bleStatusCharacteristic->notify();
}

void queueBleRequest(BleRequestType type, const String &value) {
  if (!bleQueue) return;
  BleRequest request{};
  request.type = type;
  const size_t length = min(value.length(), static_cast<size_t>(MAX_BLE_JSON));
  memcpy(request.payload, value.c_str(), length);
  request.payload[length] = '\0';
  if (xQueueSend(bleQueue, &request, 0) != pdTRUE) publishBleStatus("busy");
}

class BleConfigCallbacks final : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic *characteristic) override {
    characteristic->setValue(buildBleConfigJson());
  }

  void onWrite(BLECharacteristic *characteristic) override {
    queueBleRequest(BleRequestType::Config, characteristic->getValue());
  }
};

class BleCommandCallbacks final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    queueBleRequest(BleRequestType::Command, characteristic->getValue());
  }
};

class BleServerCallbacks final : public BLEServerCallbacks {
  void onConnect(BLEServer *) override {
    bleClientConnected = true;
  }

  void onDisconnect(BLEServer *) override {
    bleClientConnected = false;
    BLEDevice::startAdvertising();
  }
};

void startBleProvisioning() {
  bleQueue = xQueueCreate(4, sizeof(BleRequest));
  const uint64_t mac = ESP.getEfuseMac();
  blePin = 100000 + static_cast<uint32_t>(mac % 900000);
  char name[28];
  // Keep the complete name short enough for the primary 31-byte advertising
  // packet. Some phone system Bluetooth pages ignore names found only in the
  // scan response, especially while Wi-Fi and BLE coexist.
  snprintf(name, sizeof(name), "ESP32-AI-%04X", static_cast<uint16_t>(mac & 0xFFFF));
  bleDeviceName = name;

  BLEDevice::init(name);
  BLEDevice::setMTU(517);
  BLEDevice::setPower(ESP_PWR_LVL_P9);
  BLESecurity *security = new BLESecurity();
  security->setPassKey(true, blePin);
  security->setCapability(ESP_IO_CAP_OUT);
  security->setAuthenticationMode(true, true, true);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new BleServerCallbacks());
  server->advertiseOnDisconnect(true);
  BLEService *service = server->createService(BLE_SERVICE_UUID);
  const uint32_t secureReadWrite = BLECharacteristic::PROPERTY_READ |
                                   BLECharacteristic::PROPERTY_WRITE |
                                   BLECharacteristic::PROPERTY_READ_AUTHEN |
                                   BLECharacteristic::PROPERTY_WRITE_AUTHEN;
  const uint32_t secureWrite = BLECharacteristic::PROPERTY_WRITE |
                               BLECharacteristic::PROPERTY_WRITE_AUTHEN;
  const uint32_t secureReadNotify = BLECharacteristic::PROPERTY_READ |
                                    BLECharacteristic::PROPERTY_NOTIFY |
                                    BLECharacteristic::PROPERTY_READ_AUTHEN;

  bleConfigCharacteristic = service->createCharacteristic(BLE_CONFIG_UUID, secureReadWrite);
  bleConfigCharacteristic->setAccessPermissions(
      ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM);
  bleConfigCharacteristic->setCallbacks(new BleConfigCallbacks());
  bleConfigCharacteristic->setValue(buildBleConfigJson());

  BLECharacteristic *command = service->createCharacteristic(BLE_COMMAND_UUID, secureWrite);
  command->setAccessPermissions(ESP_GATT_PERM_WRITE_ENC_MITM);
  command->setCallbacks(new BleCommandCallbacks());

  bleStatusCharacteristic = service->createCharacteristic(BLE_STATUS_UUID, secureReadNotify);
  bleStatusCharacteristic->setAccessPermissions(ESP_GATT_PERM_READ_ENC_MITM);
  bleStatusCharacteristic->addDescriptor(new BLE2902());
  bleStatusCharacteristic->setValue(buildBleStatusJson("ready"));

  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  BLEAdvertisementData primaryAdvertisement;
  primaryAdvertisement.setFlags(0x06);  // General discoverable, BLE-only.
  primaryAdvertisement.setName(name);
  advertising->setAdvertisementData(primaryAdvertisement);

  BLEAdvertisementData scanResponse;
  scanResponse.setCompleteServices(BLEUUID(BLE_SERVICE_UUID));
  advertising->setScanResponseData(scanResponse);
  advertising->setScanResponse(true);
  advertising->setMinInterval(0x80);  // 80 ms, responsive without starving Wi-Fi.
  advertising->setMaxInterval(0xA0);  // 100 ms.
  advertising->setMinPreferred(0x06);
  advertising->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[BLE] %s 持续可用，配对 PIN=%06lu。\n", name,
                static_cast<unsigned long>(blePin));
}

bool applyBleConfig(const String &payload, bool &connectRequested) {
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, payload);
  if (error || !doc.is<JsonObject>()) {
    publishBleStatus("invalid_json");
    return false;
  }

  if (doc["ssid"].is<const char *>()) settings.wifiSsid = doc["ssid"].as<String>();
  if (doc["wifi_password"].is<const char *>()) {
    settings.wifiPassword = doc["wifi_password"].as<String>();
  }
  if (doc["server_url"].is<const char *>()) {
    settings.serverUrl = normalizedServerUrl(doc["server_url"].as<String>());
  } else if (doc["lan_server"].is<const char *>()) {
    settings.serverUrl = normalizedServerUrl(doc["lan_server"].as<String>());
  }
  if (doc["discovery_port"].is<int>()) {
    const int port = doc["discovery_port"].as<int>();
    if (port < 1 || port > 65535) {
      publishBleStatus("invalid_discovery_port");
      return false;
    }
    settings.discoveryPort = static_cast<uint16_t>(port);
  }
  if (doc["volume_percent"].is<int>()) {
    settings.volumePercent = constrain(doc["volume_percent"].as<int>(), 0, 100);
    settings.volumePotentiometer = false;
  }
  if (doc["volume_mode"].is<const char *>()) {
    String mode = doc["volume_mode"].as<String>();
    mode.toLowerCase();
    if (mode != "fixed" && mode != "pot") {
      publishBleStatus("invalid_volume_mode");
      return false;
    }
    settings.volumePotentiometer = mode == "pot";
  }

  if (settings.wifiSsid.length() > 32 || settings.wifiPassword.length() > 63 ||
      settings.serverUrl.length() > 180 ||
      (!settings.serverUrl.isEmpty() && !settings.serverUrl.startsWith("http://"))) {
    publishBleStatus("invalid_config");
    return false;
  }
  connectRequested = doc["connect"] | true;
  VoiceInput::setPlaybackVolume(settings.volumePercent);
  VoiceInput::usePotentiometerVolume(settings.volumePotentiometer);
  if (!saveProvisioningSettings()) {
    publishBleStatus("save_failed");
    return false;
  }
  if (bleConfigCharacteristic) bleConfigCharacteristic->setValue(buildBleConfigJson());
  publishBleStatus("config_saved");
  return true;
}

void processBleRequests() {
  if (!bleQueue) return;
  BleRequest request{};
  while (xQueueReceive(bleQueue, &request, 0) == pdTRUE) {
    String value(request.payload);
    if (request.type == BleRequestType::Config) {
      bool connectRequested = false;
      if (applyBleConfig(value, connectRequested) && connectRequested) {
        if (connectWifi(true)) {
          if (settings.serverUrl.isEmpty() || !serverHealthCheck()) discoverServer(true);
          startPhoneWeb();
          publishBleStatus("connected");
        } else {
          publishBleStatus("wifi_failed");
        }
      }
      continue;
    }

    value.trim();
    value.toLowerCase();
    if (value == "status") publishBleStatus("status");
    else if (value == "connect") {
      const bool ok = connectWifi(true);
      if (ok && (settings.serverUrl.isEmpty() || !serverHealthCheck())) discoverServer(true);
      if (ok) startPhoneWeb();
      publishBleStatus(ok ? "connected" : "wifi_failed");
    } else if (value == "discover") {
      publishBleStatus(discoverServer(true) ? "server_discovered" : "server_not_found");
    } else if (value == "reboot") {
      publishBleStatus("rebooting");
      delay(200);
      ESP.restart();
    } else {
      publishBleStatus("unknown_command");
    }
  }
}

bool uploadDialogue(String &statusPath) {
  File recording = VoiceInput::recordingStorage().open(RECORDING_PATH, FILE_READ);
  if (!recording) {
    Serial.println(F("[局域网] 无法打开录音文件。"));
    return false;
  }

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(60000);
  if (!http.begin(settings.serverUrl + "/v1/dialogue")) {
    recording.close();
    return false;
  }
  http.addHeader("Content-Type", "application/octet-stream");
  http.addHeader("X-Sample-Rate", String(VoiceInput::SAMPLE_RATE));
  addDeviceHeaders(http);
  const int code = http.sendRequest("POST", &recording, recording.size());
  recording.close();
  const String body = http.getString();
  http.end();
  if (code != HTTP_CODE_OK && code != HTTP_CODE_ACCEPTED) {
    Serial.printf("[局域网] 对话请求失败，HTTP=%d，%s\n", code, body.substring(0, 300).c_str());
    return false;
  }

  JsonDocument response;
  const DeserializationError error = deserializeJson(response, body);
  if (error || !response["ok"].as<bool>()) {
    Serial.println(F("[局域网] 服务端返回格式无效。"));
    return false;
  }
  statusPath = response["status_path"].as<String>();
  Serial.println(F("[局域网] 录音已提交，等待离线识别、AI 与语音合成。"));
  return !statusPath.isEmpty();
}

bool waitForJob(const String &statusPath, String &audioPath, String &recognizedText,
                size_t &audioBytes) {
  const uint32_t started = millis();
  while (millis() - started < 15UL * 60UL * 1000UL) {
    if (digitalRead(VoiceInput::PIN_VOICE_BUTTON) == LOW) {
      Serial.println(F("[局域网] 用户取消等待；电脑端任务可以继续完成。"));
      return false;
    }
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(10000);
    const String url = statusPath.startsWith("http") ? statusPath : settings.serverUrl + statusPath;
    if (!http.begin(url)) return false;
    addDeviceHeaders(http);
    const int code = http.GET();
    const String body = http.getString();
    http.end();
    if (code == HTTP_CODE_OK) {
      JsonDocument state;
      if (!deserializeJson(state, body)) {
        const String status = state["status"].as<String>();
        if (status == "done") {
          audioPath = state["audio_path"].as<String>();
          recognizedText = state["recognized_text"].as<String>();
          audioBytes = state["audio_bytes"].as<size_t>();
          Serial.println("你(离线识别)> " + recognizedText);
          Serial.printf("[局域网] 回答语音 %u bytes，将直接流式播放。\n",
                        static_cast<unsigned>(audioBytes));
          return !audioPath.isEmpty() && audioBytes > 0;
        }
        if (status == "error") {
          Serial.println("[局域网] 服务端任务失败：" + state["error"].as<String>());
          return false;
        }
      }
    }
    for (uint8_t tick = 0; tick < 20; ++tick) {
      if (digitalRead(VoiceInput::PIN_VOICE_BUTTON) == LOW) return false;
      delay(50);
    }
  }
  Serial.println(F("[局域网] 等待服务端超过 15 分钟。"));
  return false;
}

bool playServerAudio(const String &audioPath, size_t expectedBytes) {
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(30000);
  const String url = audioPath.startsWith("http") ? audioPath : settings.serverUrl + audioPath;
  if (!http.begin(url)) return false;
  addDeviceHeaders(http);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[播放] 下载失败，HTTP=%d\n", code);
    http.end();
    return false;
  }
  const int announced = http.getSize();
  size_t bytes = announced > 0 ? static_cast<size_t>(announced) : expectedBytes;
  if (bytes == 0 || (bytes & 1U) != 0) {
    Serial.println(F("[播放] 服务端没有提供有效 PCM 长度。"));
    http.end();
    return false;
  }
  NetworkClient *stream = http.getStreamPtr();
  stream->setTimeout(15000);
  const bool ok = VoiceInput::playPcmStream(*stream, bytes, true);
  http.end();
  return ok;
}

bool runPushToTalkDialogue() {
  if (busy) return false;
  busy = true;
  if (WiFi.status() != WL_CONNECTED && !connectWifi()) {
    setState(DeviceState::Error);
    busy = false;
    return false;
  }
  if (!ensureServer()) {
    setState(DeviceState::Error);
    busy = false;
    return false;
  }

  setState(DeviceState::Recording);
  Serial.println(F("[按键对话] 正在录音；松开 BOOT 后发送，最长 20 秒。"));
  VoiceInput::AudioStats stats;
  const bool recorded = VoiceInput::recordPcmPushToTalk(
      RECORDING_PATH, RECORD_MINIMUM_MS, RECORD_MAXIMUM_MS, stats);
  if (!recorded || stats.rms < 80 || stats.peak < 250) {
    Serial.printf("[按键对话] 录音无效，RMS=%u，Peak=%u\n", stats.rms, stats.peak);
    setState(DeviceState::Error);
    busy = false;
    return false;
  }
  Serial.printf("[按键对话] 录音 %lu ms，%u bytes，RMS=%u，Peak=%u\n",
                static_cast<unsigned long>(stats.durationMs), static_cast<unsigned>(stats.fileBytes),
                stats.rms, stats.peak);

  setState(DeviceState::Processing);
  String statusPath;
  String audioPath;
  String recognizedText;
  size_t audioBytes = 0;
  if (!uploadDialogue(statusPath) ||
      !waitForJob(statusPath, audioPath, recognizedText, audioBytes)) {
    setState(DeviceState::Error);
    busy = false;
    return false;
  }

  setState(DeviceState::Playing);
  Serial.println(F("[播放] 正在播放；按 BOOT 可停止。"));
  const bool played = playServerAudio(audioPath, audioBytes);
  Serial.println(played ? F("[播放] 本轮完成。") : F("[播放] 本轮失败。"));
  setState(played ? DeviceState::Idle : DeviceState::Error);
  busy = false;
  return played;
}

void printStatus() {
  Serial.println(F("\n[局域网独立设备状态]"));
  Serial.printf("  Wi-Fi: %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  Serial.println("  SSID: " + settings.wifiSsid);
  if (WiFi.status() == WL_CONNECTED) Serial.println("  IP: " + WiFi.localIP().toString());
  if (WiFi.status() == WL_CONNECTED) Serial.println("  Phone page: http://" + WiFi.localIP().toString() + "/");
  Serial.println("  Server: " + (settings.serverUrl.isEmpty() ? String("auto-discovery") : settings.serverUrl));
  Serial.printf("  Discovery UDP: %u\n", settings.discoveryPort);
  if (settings.volumePotentiometer) {
    Serial.printf("  Volume: potentiometer GPIO%d (now about %u%%)\n",
                  VoiceInput::PIN_VOLUME_POT, VoiceInput::readPotentiometerVolume());
  } else {
    Serial.printf("  Volume: fixed %u%%\n", settings.volumePercent);
  }
  Serial.printf("  I2S: BCLK=%d WS=%d MIC_SD=%d AMP_DIN=%d AMP_SD=%d\n",
                VoiceInput::PIN_I2S_BCLK, VoiceInput::PIN_I2S_WS, VoiceInput::PIN_MIC_DATA,
                VoiceInput::PIN_AMP_DATA, VoiceInput::PIN_AMP_SD);
  Serial.printf("  Recording storage: %s\n", VoiceInput::recordingStorageName());
  Serial.printf("  BLE: %s, pairing PIN=%06lu\n", bleDeviceName.c_str(),
                static_cast<unsigned long>(blePin));
  Serial.println(F("  Runtime does not require this serial connection."));
}

void processConsoleLine(String line) {
  line.trim();
  if (line.isEmpty()) return;
  if (line == "/status") printStatus();
  else if (line == "/discover") discoverServer(true);
  else if (line == "/voice") {
    Serial.println(F("[提示] 控制台触发时请立即按住 BOOT 说话，松开结束。"));
    runPushToTalkDialogue();
  }
  else if (line == "/server") Serial.println("[局域网] " + settings.serverUrl);
  else if (line.startsWith("/server ")) {
    settings.serverUrl = normalizedServerUrl(line.substring(8));
    if (!settings.serverUrl.startsWith("http://") && !settings.serverUrl.startsWith("https://")) {
      Serial.println(F("[局域网] 地址必须以 http:// 或 https:// 开头。"));
    } else {
      saveLanSettings();
      Serial.println("[局域网] 已保存：" + settings.serverUrl);
    }
  }
  else if (line.startsWith("/volume ")) {
    String value = line.substring(8);
    value.trim();
    value.toLowerCase();
    if (value == "pot") {
      settings.volumePotentiometer = true;
      VoiceInput::usePotentiometerVolume(true);
      saveLanSettings();
      Serial.printf("[音量] 已启用 GPIO%d 电位器并保存。\n", VoiceInput::PIN_VOLUME_POT);
    } else if (value == "fixed") {
      settings.volumePotentiometer = false;
      VoiceInput::usePotentiometerVolume(false);
      saveLanSettings();
      Serial.printf("[音量] 已切回固定音量 %u%%。\n", settings.volumePercent);
    } else {
      bool numeric = !value.isEmpty();
      for (size_t i = 0; i < value.length(); ++i) {
        if (!isDigit(static_cast<unsigned char>(value[i]))) numeric = false;
      }
      const int percent = numeric ? value.toInt() : -1;
      if (percent < 0 || percent > 100) {
        Serial.println(F("[音量] 用法：/volume 0..100、/volume pot 或 /volume fixed。"));
        return;
      }
      settings.volumePercent = static_cast<uint8_t>(percent);
      settings.volumePotentiometer = false;
      VoiceInput::setPlaybackVolume(settings.volumePercent);
      VoiceInput::usePotentiometerVolume(false);
      saveLanSettings();
      Serial.printf("[音量] 已保存 %u%%。\n", settings.volumePercent);
    }
  }
  else if (line == "/connect") connectWifi();
  else if (line == "/reboot") ESP.restart();
  else Serial.println(F("[命令] /status /discover /server [URL] /volume 0..100|pot|fixed /voice /connect /reboot"));
}

void serviceConsole() {
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      processConsoleLine(serialLine);
      serialLine = "";
    } else if (serialLine.length() < MAX_CONSOLE_LINE) {
      serialLine += c;
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(350);
  setState(DeviceState::Booting);
  loadSettings();
  startBleProvisioning();
  if (!VoiceInput::begin()) {
    Serial.println(F("[启动] I2S/录音文件系统初始化失败。"));
    setState(DeviceState::Error);
    return;
  }
  previousButton = digitalRead(VoiceInput::PIN_VOICE_BUTTON);
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    if (!serverHealthCheck()) discoverServer(true);
    startPhoneWeb();
  }
  previousWifiStatus = WiFi.status();
  publishBleStatus("ready");
  setState(DeviceState::Idle);
  Serial.println(F("\nESP32-S3 局域网独立语音设备已启动。"));
  Serial.println(F("按住 BOOT 说话，松开发送；播放时按 BOOT 停止。串口可拔除。"));
  printStatus();
}

void loop() {
  if (!webStarted && WiFi.status() == WL_CONNECTED) startPhoneWeb();
  if (webStarted) phoneWeb.handleClient();
  serviceConsole();
  processBleRequests();
  const wl_status_t currentWifiStatus = WiFi.status();
  if (currentWifiStatus != previousWifiStatus || millis() - lastBleStatusMs > 10000) {
    previousWifiStatus = currentWifiStatus;
    lastBleStatusMs = millis();
    publishBleStatus(currentWifiStatus == WL_CONNECTED ? "online" : "offline");
  }
  const bool currentButton = digitalRead(VoiceInput::PIN_VOICE_BUTTON);
  if (!busy && previousButton == HIGH && currentButton == LOW) {
    delay(28);
    if (digitalRead(VoiceInput::PIN_VOICE_BUTTON) == LOW) runPushToTalkDialogue();
  }
  previousButton = digitalRead(VoiceInput::PIN_VOICE_BUTTON);
  delay(8);
}

}  // namespace LanDevice

void setup() { LanDevice::setup(); }
void loop() { LanDevice::loop(); }
