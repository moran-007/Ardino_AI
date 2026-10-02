#include "firmware_update.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SHA2Builder.h>
#include <Update.h>
#include <WiFi.h>
#if __has_include(<NetworkClientSecure.h>)
#include <NetworkClientSecure.h>
using OtaSecureClient = NetworkClientSecure;
#else
#include <WiFiClientSecure.h>
using OtaSecureClient = WiFiClientSecure;
#endif
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>

#include "firmware_public_key.h"

namespace FirmwareUpdate {
namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;
constexpr uint32_t STREAM_STALL_TIMEOUT_MS = 20000;
constexpr size_t MAX_MANIFEST_BYTES = 4096;
constexpr size_t DOWNLOAD_BUFFER_BYTES = 4096;
constexpr size_t MAX_SIGNATURE_BYTES = 512;

struct Manifest {
  String version;
  String hardware;
  String url;
  String sha256;
  String signature;
  size_t size = 0;
  bool force = false;
};

void report(Reporter reporter, const String &message) {
  if (reporter) reporter(message);
}

Outcome outcome(Result result, const String &message, const String &version = "") {
  Outcome value;
  value.result = result;
  value.message = message;
  value.availableVersion = version;
  return value;
}

bool isStrictHttpsUrl(const String &url) {
  if (!url.startsWith("https://") || url.length() > 512) return false;
  const int hostStart = 8;
  const int pathStart = url.indexOf('/', hostStart);
  const String authority = pathStart < 0 ? url.substring(hostStart) : url.substring(hostStart, pathStart);
  return !authority.isEmpty() && authority.indexOf('@') < 0 && authority.indexOf('#') < 0 &&
         authority.indexOf('?') < 0;
}

bool isSha256Hex(const String &value) {
  if (value.length() != 64) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
  }
  return true;
}

bool parseVersionPart(const String &version, int &offset, uint32_t &part) {
  if (offset >= static_cast<int>(version.length())) return false;
  uint64_t value = 0;
  int digits = 0;
  while (offset < static_cast<int>(version.length()) && isDigit(version[offset])) {
    value = value * 10 + static_cast<uint8_t>(version[offset] - '0');
    if (value > 999999) return false;
    ++offset;
    ++digits;
  }
  if (digits == 0) return false;
  part = static_cast<uint32_t>(value);
  return true;
}

bool parseSemanticVersion(const String &version, uint32_t parts[3]) {
  int offset = 0;
  for (int i = 0; i < 3; ++i) {
    if (!parseVersionPart(version, offset, parts[i])) return false;
    if (i < 2) {
      if (offset >= static_cast<int>(version.length()) || version[offset] != '.') return false;
      ++offset;
    }
  }
  return offset == static_cast<int>(version.length());
}

int compareSemanticVersions(const String &left, const String &right) {
  uint32_t leftParts[3]{};
  uint32_t rightParts[3]{};
  if (!parseSemanticVersion(left, leftParts) || !parseSemanticVersion(right, rightParts)) return 0;
  for (int i = 0; i < 3; ++i) {
    if (leftParts[i] < rightParts[i]) return -1;
    if (leftParts[i] > rightParts[i]) return 1;
  }
  return 0;
}

String canonicalManifest(const Manifest &manifest) {
  return manifest.version + "\n" + manifest.hardware + "\n" + String(manifest.size) + "\n" +
         manifest.sha256 + "\n" + manifest.url + "\n" + (manifest.force ? "true" : "false");
}

String httpsOrigin(const String &url) {
  if (!isStrictHttpsUrl(url)) return "";
  const int pathStart = url.indexOf('/', 8);
  String origin = pathStart < 0 ? url : url.substring(0, pathStart);
  origin.toLowerCase();
  return origin;
}

bool verifyManifestSignature(const Manifest &manifest) {
  uint8_t signature[MAX_SIGNATURE_BYTES]{};
  size_t signatureLength = 0;
  const int decodeResult = mbedtls_base64_decode(
      signature, sizeof(signature), &signatureLength,
      reinterpret_cast<const unsigned char *>(manifest.signature.c_str()), manifest.signature.length());
  if (decodeResult != 0 || signatureLength == 0) return false;

  const String canonical = canonicalManifest(manifest);
  SHA256Builder hash;
  hash.begin();
  hash.add(reinterpret_cast<const uint8_t *>(canonical.c_str()), canonical.length());
  hash.calculate();
  uint8_t digest[32]{};
  hash.getBytes(digest);

  mbedtls_pk_context publicKey;
  mbedtls_pk_init(&publicKey);
  const int parseResult = mbedtls_pk_parse_public_key(
      &publicKey, reinterpret_cast<const unsigned char *>(OTA_FIRMWARE_SIGNING_PUBLIC_KEY),
      sizeof(OTA_FIRMWARE_SIGNING_PUBLIC_KEY));
  if (parseResult != 0) {
    mbedtls_pk_free(&publicKey);
    return false;
  }
  const int verifyResult = mbedtls_pk_verify(
      &publicKey, MBEDTLS_MD_SHA256, digest, sizeof(digest), signature, signatureLength);
  mbedtls_pk_free(&publicKey);
  return verifyResult == 0;
}

bool parseManifest(const String &payload, Manifest &manifest) {
  if (payload.isEmpty() || payload.length() > MAX_MANIFEST_BYTES) return false;
  JsonDocument document;
  if (deserializeJson(document, payload) || !document.is<JsonObject>()) return false;
  if (!document["version"].is<const char *>() || !document["hardware"].is<const char *>() ||
      !document["url"].is<const char *>() || !document["sha256"].is<const char *>() ||
      !document["signature"].is<const char *>() || !document["size"].is<size_t>()) return false;

  manifest.version = document["version"].as<String>();
  manifest.hardware = document["hardware"].as<String>();
  manifest.url = document["url"].as<String>();
  manifest.sha256 = document["sha256"].as<String>();
  manifest.sha256.toLowerCase();
  manifest.signature = document["signature"].as<String>();
  manifest.size = document["size"].as<size_t>();
  manifest.force = document["force"] | false;

  uint32_t versionParts[3]{};
  return parseSemanticVersion(manifest.version, versionParts) &&
         manifest.hardware == HARDWARE_ID && isStrictHttpsUrl(manifest.url) &&
         isSha256Hex(manifest.sha256) && !manifest.signature.isEmpty() &&
         manifest.size >= 65536 && manifest.size <= 0x300000;
}

void addDeviceHeaders(HTTPClient &http, const Config &config) {
  http.addHeader("X-Device-ID", config.deviceId);
  http.addHeader("Authorization", "Bearer " + config.deviceToken);
  http.addHeader("X-Firmware-Version", config.currentVersion);
  http.addHeader("X-Hardware-ID", HARDWARE_ID);
}

Outcome downloadAndInstall(const Config &config, const Manifest &manifest, Reporter reporter) {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
  if (!next || next == running || next->size < manifest.size) {
    return outcome(Result::PartitionError, "没有可用的备用 OTA 分区", manifest.version);
  }

  OtaSecureClient secureClient;
  secureClient.setCACert(config.caCertificate);
  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(REQUEST_TIMEOUT_MS);
  http.useHTTP10(true);
  if (!http.begin(secureClient, manifest.url)) {
    return outcome(Result::DownloadError, "无法创建固件下载请求", manifest.version);
  }
  addDeviceHeaders(http, config);
  const int status = http.GET();
  if (status != HTTP_CODE_OK) {
    http.end();
    secureClient.stop();
    return outcome(Result::DownloadError, "固件下载 HTTP " + String(status), manifest.version);
  }
  const int contentLength = http.getSize();
  if (contentLength >= 0 && static_cast<size_t>(contentLength) != manifest.size) {
    http.end();
    secureClient.stop();
    return outcome(Result::DownloadError, "固件 Content-Length 与清单不一致", manifest.version);
  }
  if (!Update.begin(manifest.size, U_FLASH)) {
    const String detail = Update.errorString();
    http.end();
    secureClient.stop();
    return outcome(Result::PartitionError, "OTA 分区初始化失败: " + detail, manifest.version);
  }

  SHA256Builder hash;
  hash.begin();
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[DOWNLOAD_BUFFER_BYTES];
  size_t received = 0;
  size_t lastReported = 0;
  uint32_t lastDataAt = millis();
  bool streamFailed = false;

  while (received < manifest.size) {
    const int available = stream->available();
    if (available <= 0) {
      if (!secureClient.connected() || millis() - lastDataAt > STREAM_STALL_TIMEOUT_MS) {
        streamFailed = true;
        break;
      }
      delay(5);
      continue;
    }
    const size_t wanted = min(
        min(static_cast<size_t>(available), sizeof(buffer)), manifest.size - received);
    const int count = stream->readBytes(buffer, wanted);
    if (count <= 0) {
      streamFailed = true;
      break;
    }
    lastDataAt = millis();
    hash.add(buffer, static_cast<size_t>(count));
    if (Update.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) {
      streamFailed = true;
      break;
    }
    received += static_cast<size_t>(count);
    if (received - lastReported >= 262144 || received == manifest.size) {
      report(reporter, "固件下载 " + String((received * 100) / manifest.size) + "%");
      lastReported = received;
    }
    delay(0);
  }
  http.end();
  secureClient.stop();

  if (streamFailed || received != manifest.size) {
    Update.abort();
    return outcome(Result::DownloadError,
                   "固件流中断: " + String(received) + "/" + String(manifest.size),
                   manifest.version);
  }

  hash.calculate();
  String actualSha256 = hash.toString();
  actualSha256.toLowerCase();
  if (actualSha256 != manifest.sha256) {
    Update.abort();
    return outcome(Result::IntegrityError, "固件 SHA-256 校验失败", manifest.version);
  }
  if (!Update.end(true)) {
    return outcome(Result::WriteError, "固件激活失败: " + String(Update.errorString()), manifest.version);
  }
  return outcome(Result::Installed, "新固件已写入备用分区", manifest.version);
}

}  // namespace

Outcome checkAndInstall(const Config &config, Reporter reporter) {
  if (WiFi.status() != WL_CONNECTED || config.deviceId.isEmpty() ||
      config.deviceToken.isEmpty() || !config.caCertificate) {
    return outcome(Result::NotConfigured, "Wi-Fi、设备凭证或 CA 未就绪");
  }
  uint32_t currentVersionParts[3]{};
  if (!parseSemanticVersion(config.currentVersion, currentVersionParts) ||
      !isStrictHttpsUrl(config.manifestUrl)) {
    return outcome(Result::NotConfigured, "本机版本或 OTA 清单地址无效");
  }

  report(reporter, "正在检查线上固件");
  OtaSecureClient secureClient;
  secureClient.setCACert(config.caCertificate);
  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(REQUEST_TIMEOUT_MS);
  if (!http.begin(secureClient, config.manifestUrl)) {
    return outcome(Result::NetworkError, "无法创建 OTA 清单请求");
  }
  addDeviceHeaders(http, config);
  const int status = http.GET();
  if (status == HTTP_CODE_NO_CONTENT || status == HTTP_CODE_NOT_MODIFIED) {
    http.end();
    secureClient.stop();
    return outcome(Result::NoUpdate, "服务器确认已是最新版本");
  }
  if (status != HTTP_CODE_OK) {
    http.end();
    secureClient.stop();
    return outcome(Result::NetworkError, "OTA 清单 HTTP " + String(status));
  }
  const int manifestLength = http.getSize();
  if (manifestLength <= 0 || manifestLength > static_cast<int>(MAX_MANIFEST_BYTES)) {
    http.end();
    secureClient.stop();
    return outcome(Result::ManifestRejected, "OTA 清单必须提供有效且不超过 4 KiB 的 Content-Length");
  }
  const String payload = http.getString();
  http.end();
  secureClient.stop();
  if (payload.length() != static_cast<size_t>(manifestLength)) {
    return outcome(Result::ManifestRejected, "OTA 清单长度不完整");
  }

  Manifest manifest;
  if (!parseManifest(payload, manifest)) {
    return outcome(Result::ManifestRejected, "OTA 清单字段无效");
  }
  if (!verifyManifestSignature(manifest)) {
    return outcome(Result::ManifestRejected, "OTA 清单 RSA 签名无效", manifest.version);
  }
  if (httpsOrigin(manifest.url) != httpsOrigin(config.manifestUrl)) {
    return outcome(Result::ManifestRejected, "固件下载地址与 OTA 服务不是同一 HTTPS 源", manifest.version);
  }
  // Never reflash the running version or downgrade. In particular, a signed
  // force flag must not create an install/reboot loop after every boot.
  if (compareSemanticVersions(manifest.version, config.currentVersion) <= 0) {
    return outcome(Result::NoUpdate, "当前版本已是最新", manifest.version);
  }

  report(reporter, "发现新版本 " + manifest.version);
  if (!config.installAvailable ||
      (!config.deferredVersion.isEmpty() && config.deferredVersion == manifest.version)) {
    const String message = config.deferredVersion == manifest.version
                               ? "版本 " + manifest.version + " 已暂缓，本次不安装"
                               : "发现可用更新 " + manifest.version + "，等待用户确认安装";
    return outcome(Result::UpdateAvailable, message, manifest.version);
  }
  return downloadAndInstall(config, manifest, reporter);
}

bool confirmRunningImage() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (!running) return false;
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  const esp_err_t stateResult = esp_ota_get_state_partition(running, &state);
  if (stateResult != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) return true;
  return esp_ota_mark_app_valid_cancel_rollback() == ESP_OK;
}

const char *resultName(Result result) {
  switch (result) {
    case Result::NoUpdate: return "no_update";
    case Result::UpdateAvailable: return "update_available";
    case Result::Installed: return "installed";
    case Result::NotConfigured: return "not_configured";
    case Result::NetworkError: return "network_error";
    case Result::ManifestRejected: return "manifest_rejected";
    case Result::PartitionError: return "partition_error";
    case Result::DownloadError: return "download_error";
    case Result::IntegrityError: return "integrity_error";
    case Result::WriteError: return "write_error";
  }
  return "unknown";
}

}  // namespace FirmwareUpdate
