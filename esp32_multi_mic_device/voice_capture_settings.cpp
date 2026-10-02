#include "voice_capture_settings.h"

#include <Preferences.h>

namespace VoiceCapture {
namespace {
Settings current;

bool valid(const Settings &value) {
  return value.gain >= 1 && value.gain <= 12 && value.sensitivity <= 100 &&
         value.silenceMs >= 600 && value.silenceMs <= 3000 &&
         value.listenSeconds >= 3 && value.listenSeconds <= 20;
}

uint64_t pack(const Settings &value) {
  return (uint64_t{1} << 56) | value.gain |
         (uint64_t{value.sensitivity} << 8) |
         (uint64_t{value.noiseSuppression} << 16) |
         (uint64_t{value.continuous} << 17) |
         (uint64_t{value.silenceMs} << 24) |
         (uint64_t{value.listenSeconds} << 40);
}

template <typename T>
bool readNumber(JsonVariantConst doc, const char *key, T &target,
                int minimum, int maximum) {
  const JsonVariantConst value = doc[key];
  if (value.isUnbound()) return true;
  if (!value.is<int>()) return false;
  const int number = value.as<int>();
  if (number < minimum || number > maximum) return false;
  target = static_cast<T>(number);
  return true;
}

bool readBool(JsonVariantConst doc, const char *key, bool &target) {
  const JsonVariantConst value = doc[key];
  if (value.isUnbound()) return true;
  if (!value.is<bool>()) return false;
  target = value.as<bool>();
  return true;
}
}  // namespace

const Settings &settings() { return current; }

bool begin() {
  current = Settings{};
  Preferences prefs;
  if (!prefs.begin("cloud-ai", true)) return false;
  const bool present = prefs.isKey("voice_capture");
  const uint64_t raw = prefs.getULong64("voice_capture", 0);
  prefs.end();
  if (!present) return true;
  Settings value;
  value.gain = raw & 0xffU;
  value.sensitivity = (raw >> 8) & 0xffU;
  value.noiseSuppression = (raw & (uint64_t{1} << 16)) != 0;
  value.continuous = (raw & (uint64_t{1} << 17)) != 0;
  value.silenceMs = (raw >> 24) & 0xffffU;
  value.listenSeconds = (raw >> 40) & 0xffU;
  if (!valid(value) || pack(value) != raw) return false;
  current = value;
  return true;
}

bool save(const Settings &candidate) {
  if (!valid(candidate)) return false;
  Preferences prefs;
  if (!prefs.begin("cloud-ai", false)) return false;
  const bool ok = prefs.putULong64("voice_capture", pack(candidate)) == sizeof(uint64_t);
  prefs.end();
  if (ok) current = candidate;
  return ok;
}

bool parse(JsonVariantConst doc, Settings &candidate) {
  if (!doc.is<JsonObjectConst>()) return false;
  Settings next = candidate;
  if (!readNumber(doc, "mic_gain", next.gain, 1, 12) ||
      !readNumber(doc, "mic_sensitivity", next.sensitivity, 0, 100) ||
      !readNumber(doc, "silence_ms", next.silenceMs, 600, 3000) ||
      !readNumber(doc, "listen_seconds", next.listenSeconds, 3, 20) ||
      !readBool(doc, "noise_suppression", next.noiseSuppression) ||
      !readBool(doc, "continuous_conversation", next.continuous)) return false;
  candidate = next;
  return true;
}

void appendStatus(JsonObject doc) {
  doc["mic_gain"] = current.gain;
  doc["mic_sensitivity"] = current.sensitivity;
  doc["noise_suppression"] = current.noiseSuppression;
  doc["silence_ms"] = current.silenceMs;
  doc["listen_seconds"] = current.listenSeconds;
  doc["continuous_conversation"] = current.continuous;
}
}  // namespace VoiceCapture
