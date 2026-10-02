#pragma once

#include <Arduino.h>

namespace FirmwareUpdate {

constexpr char HARDWARE_ID[] = "esp32s3-cloud-voice-multimic-v1";

enum class Result : uint8_t {
  NoUpdate,
  UpdateAvailable,
  Installed,
  NotConfigured,
  NetworkError,
  ManifestRejected,
  PartitionError,
  DownloadError,
  IntegrityError,
  WriteError,
};

struct Config {
  String manifestUrl;
  String deviceId;
  String deviceToken;
  String currentVersion;
  String deferredVersion;
  const char *caCertificate = nullptr;
  bool installAvailable = true;
};

struct Outcome {
  Result result = Result::NetworkError;
  String message;
  String availableVersion;
};

using Reporter = void (*)(const String &message);

Outcome checkAndInstall(const Config &config, Reporter reporter = nullptr);
bool confirmRunningImage();
const char *resultName(Result result);

}  // namespace FirmwareUpdate
