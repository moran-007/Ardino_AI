#include "Arduino.h"
#include <ArduinoJson.h>
#include "../../custom_wake.h"
#include "../../offline_wake.h"
#include "../../web_custom_wake.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
size_t checks = 0;
bool listenerPaused = true, acknowledgePause = true, micReady = true, readFailure = false;
std::vector<int16_t> recordingPcm;
size_t readOffset = 0;
int serviceCalls = 0;
void require(bool value, const char *message) {
  ++checks; if (!value) throw std::runtime_error(message);
}
JsonDocument state() {
  JsonDocument result;
  const String json = CustomWake::toJson();
  require(!deserializeJson(result, json.c_str()), "module must provide valid JSON");
  return result;
}
std::vector<int16_t> phrase(float stretch = 1, float gain = 1, bool other = false,
                          bool reverse = false, int noise = 0) {
  std::vector<int16_t> result;
  const size_t speech = static_cast<size_t>(16000 * 0.84f * stretch);
  result.resize(1600 + speech + 7000);
  uint32_t seed = 7;
  for (size_t i = 0; i < result.size(); ++i) {
    seed = 1664525U * seed + 1013904223U;
    float value = static_cast<int>(seed >> 16) / 32768.0f - 1;
    value *= noise;
    if (i >= 1600 && i < 1600 + speech) {
      const size_t k = i - 1600;
      int part = static_cast<int>(k * 3 / speech);
      if (reverse) part = 2 - part;
      const float f1[] = {390, 790, 1240}, f2[] = {1170, 1730, 2470}, f3[] = {2200, 3230, 4500};
      const double t = k / 16000.0;
      const float base = other ? 2600 : f1[part];
      const float middle = other ? 4800 : f2[part];
      const float high = other ? 5900 : f3[part];
      const float envelope = 0.75f + 0.25f * sinf(static_cast<float>(k) * 0.0008f);
      value += gain * envelope * (5000 * sin(6.2831853 * base * t) +
                  2500 * sin(6.2831853 * middle * t) + 1000 * sin(6.2831853 * high * t));
    }
    result[i] = static_cast<int16_t>(max(-32768.0f, min(32767.0f, value)));
  }
  return result;
}
void prepare(std::vector<int16_t> pcm) {
  recordingPcm = std::move(pcm);recordingPcm.resize(48000);readOffset = 0;
  readFailure = false; serviceCalls = 0;
}
void service() {
  ++serviceCalls;
  require(listenerPaused, "training service must run after pause acknowledgement");
  if (serviceCalls == 1) {
    require(CustomWake::trainingActive(), "callback must observe foreground recording state");
    require(state()["training"] == "recording", "poll must report recording");
    require(!CustomWake::setLabel("不可修改"), "label must lock during enrollment");
    require(!CustomWake::enable(true), "enable must lock during enrollment");
    require(!CustomWake::reset(), "clear must lock during enrollment");
    require(!CustomWake::queueTraining(), "second training must not queue during capture");
  }
}
void cancelService() { if (++serviceCalls == 10) CustomWake::cancelTraining(); }
bool detect(const std::vector<int16_t> &pcm) {
  CustomWake::resetDetection();
  size_t detected = 0;
  int16_t frame[320] = {};
  for (size_t start = 0; start < pcm.size() + 6400; start += 320) {
    for (size_t i = 0; i < 320; ++i) frame[i] = start + i < pcm.size() ? pcm[start + i] : 0;
    if (CustomWake::process(frame, 320)) ++detected;
  }
  require(detected <= 1, "one utterance must not repeatedly fire");
  return detected == 1;
}
}

namespace OfflineWake {
void suspend() { if (acknowledgePause) listenerPaused = true; }
bool paused() { return listenerPaused; }
void resume() { listenerPaused = false; }
}
namespace VoiceInput {
bool ready() { return micReady; }
bool discardMicrophone(uint32_t) { return micReady && listenerPaused; }
bool readMicrophoneMono16(int16_t *out, size_t count, uint32_t) {
  require(listenerPaused, "foreground enrollment raced listener microphone access");
  if (readFailure) return false;
  if (readOffset + count > recordingPcm.size()) return false;
  memcpy(out, recordingPcm.data() + readOffset, count * 2);
  readOffset += count;fakeMillis += 20;return true;
}
}

int main(int argc, char **argv) {
  try {
    fs::FS storage;
    require(CustomWake::begin(storage), "fresh store must initialize");
    require(!CustomWake::enabled() && !CustomWake::ready(), "custom wake must default disabled/untrained");
    require(!CustomWake::enable(true), "untrained feature must not enable");
    require(CustomWake::setLabel("小默小默"), "arbitrary wake label must save");
    require(!CustomWake::setLabel("   "), "empty label must fail");
    require(!CustomWake::setLabel(String(std::string(61, 'a'))), "oversized label must fail");
    require(!CustomWake::setLabel("bad\nlabel"), "control characters must fail");
    micReady = false;
    require(!CustomWake::queueTraining(), "unready mic must reject enrollment");micReady = true;
    prepare(phrase());
    require(CustomWake::queueTraining(), "training must queue in idle state");
    require(state()["training"] == "queued", "HTTP result must describe queued capture");
    require(CustomWake::processQueuedTraining(service), "valid acoustic phrase must enroll");
    require(serviceCalls == 150, "capture must service status/cancel through three-second recording");
    require(CustomWake::sampleCount() == 1 && !CustomWake::enabled(), "successful training must not auto-enable");
    require(state()["training"] == "success", "completed queue must expose success");
    require(CustomWake::enable(true), "trained custom wake must enable");
    require(detect(phrase()), "same acoustic phrase must detect");
    require(detect(phrase(1.1f, 0.58f, false, false, 55)), "gain/tempo/noise variant must detect");
    require(!detect(phrase(1, 1, true)), "unrelated spectral phrase must not detect");
    require(!detect(phrase(1, 1, false, true)), "reversed syllables must not detect");
    require(!detect(std::vector<int16_t>(48000)), "silence must not wake");
    std::vector<int16_t> noise(48000);uint32_t seed = 5;
    for (auto &v : noise) { seed = 1664525U * seed + 1013904223U;v = static_cast<int16_t>((seed >> 16) % 1400 - 700); }
    require(!detect(noise), "background noise must not wake");
    require(CustomWake::begin(storage) && CustomWake::enabled() && CustomWake::sampleCount() == 1,
            "FS reload must preserve label, enabled setting and acoustic templates");
    require(String(CustomWake::label()) == "小默小默" && detect(phrase(1.08f, 0.7f)),
            "persisted features must still detect user phrase");
    const auto original = *wakeFiles.at("/custom_wake.bin");
    prepare(phrase());shortWakeWrite = true;
    require(!CustomWake::recordSample(), "short writes must fail enrollment");shortWakeWrite = false;
    require(CustomWake::sampleCount() == 1 && *wakeFiles.at("/custom_wake.bin") == original,
            "short write must preserve count and committed templates");
    failWakeRename = true;
    require(!CustomWake::setLabel("新唤醒词"), "rename failure must reject label update");failWakeRename = false;
    require(String(CustomWake::label()) == "小默小默", "failed label commit must retain trained label");
    failWakeWrite = true;
    require(!CustomWake::reset(), "write failure must reject clear");failWakeWrite = false;
    require(CustomWake::sampleCount() == 1 && CustomWake::enabled(), "failed clear must preserve live configuration");
    prepare(phrase());
    require(!CustomWake::recordSample(cancelService), "cancel must stop in-flight enrollment");
    require(state()["training"] == "cancelled" && CustomWake::sampleCount() == 1,
            "cancel must expose result and retain samples");
    prepare(std::vector<int16_t>(48000));
    require(!CustomWake::recordSample() && CustomWake::sampleCount() == 1, "silent training must fail without replacing samples");
    auto clipping = std::vector<int16_t>(48000);
    for (size_t i = 1600; i < 17000; ++i) clipping[i] = i & 1 ? 32767 : -32768;
    prepare(clipping);require(!CustomWake::recordSample(), "clipping must reject training");
    auto shortSound = phrase();shortSound.resize(3000);prepare(shortSound);
    require(!CustomWake::recordSample(), "too-short sound must fail");
    auto longSound = phrase(4);prepare(longSound);
    require(!CustomWake::recordSample(), "overlong/truncated phrase must fail");
    prepare(phrase());readFailure = true;
    require(!CustomWake::recordSample(), "mic read failure must fail without samples");
    listenerPaused = false;acknowledgePause = false;prepare(phrase());
    require(!CustomWake::recordSample() && readOffset == 0, "pause timeout must never read microphone");
    acknowledgePause = true;listenerPaused = true;
    prepare(phrase(0.95f));require(CustomWake::recordSample(), "second sample must enroll");
    prepare(phrase(1.1f));require(CustomWake::recordSample(), "third sample must enroll");
    require(CustomWake::sampleCount() == 3 && !CustomWake::queueTraining(), "template count must be bounded at three");
    require(CustomWake::setLabel("换个名字") && !CustomWake::ready() && !CustomWake::enabled(),
            "changed label must invalidate old acoustic templates and disable");
    require(CustomWake::begin(storage) && CustomWake::sampleCount() == 0 && String(CustomWake::label()) == "换个名字",
            "label reset must persist");
    prepare(phrase());require(CustomWake::queueTraining(), "new label must train");
    CustomWake::cancelTraining();
    require(!CustomWake::processQueuedTraining() && readOffset == 0, "queued cancellation must skip capture");
    prepare(phrase());require(CustomWake::recordSample() && CustomWake::enable(true), "privacy recovery checks must start with saved enabled biometric template");
    const auto biometric = *wakeFiles.at("/custom_wake.bin");
    failWakePromote = true;
    require(!CustomWake::enable(false), "failed primary promotion must reject update");failWakePromote = false;
    require(*wakeFiles.at("/custom_wake.bin") == biometric && CustomWake::enabled(), "promotion failure must restore intact old primary");
    require(storage.rename("/custom_wake.bin", "/custom_wake.bak"), "interrupted rotation fixture must move primary to backup");
    require(CustomWake::begin(storage) && CustomWake::enabled() && storage.exists("/custom_wake.bin") && !storage.exists("/custom_wake.bak"), "missing primary with valid backup must recover only by successful rename");
    failWakeRemoveBackup = true;
    require(!CustomWake::reset(), "backup deletion failure must not claim successful privacy purge");
    require(!CustomWake::enabled() && !CustomWake::ready() && CustomWake::sampleCount() == 0 && state()["cleanupPending"].as<bool>(), "committed empty primary must remain authoritative while retained backup is reported");
    require(storage.exists("/custom_wake.bak") && *wakeFiles.at("/custom_wake.bak") == biometric, "remove-failure fixture must retain actual old biometric bytes");
    require(CustomWake::begin(storage) && !CustomWake::enabled() && CustomWake::sampleCount() == 0, "empty primary must not load enabled retained backup on reboot");
    wakeFiles["/custom_wake.bin"] = std::make_shared<std::string>("corrupted");
    require(!CustomWake::begin(storage) && !state()["initialized"].as<bool>() && !CustomWake::enabled() && !CustomWake::ready(), "corrupt existing primary must never resurrect retained biometric backup");
    const auto retained = *wakeFiles.at("/custom_wake.bak");failWakeWrite = true;
    require(!CustomWake::reset() && *wakeFiles.at("/custom_wake.bak") == retained, "failed repair write must preserve backup bytes without loading them");failWakeWrite = false;
    require(!CustomWake::begin(storage) && !CustomWake::enabled(), "corrupt primary plus valid backup must remain failed closed after write failure");
    failWakeRemoveBackup = false;
    require(CustomWake::reset() && !storage.exists("/custom_wake.bak") && CustomWake::sampleCount() == 0, "explicit retry reset must purge retained templates and repair corrupt primary");
    require(CustomWake::begin(storage) && !CustomWake::enabled() && !CustomWake::ready(), "successful reset must persist disabled empty state");
    wakeFiles["/custom_wake.bak"] = std::make_shared<std::string>(biometric);storage.remove("/custom_wake.bin");
    failWakeRename = true;
    require(!CustomWake::begin(storage) && !CustomWake::enabled() && !state()["initialized"].as<bool>(), "recovery rename failure must not pretend templates loaded");failWakeRename = false;
    require(CustomWake::reset() && !storage.exists("/custom_wake.bak"), "explicit reset must repair failed missing-primary recovery");
    wakeFiles["/custom_wake.bin"] = std::make_shared<std::string>("corrupted");
    require(!CustomWake::begin(storage) && !CustomWake::ready() && !CustomWake::enabled(), "corrupt file must fail closed");
    String html;WebCustomWake::appendControls(html);
    if (argc > 1) { std::ofstream file(argv[1]);file << html.c_str(); }
    std::cout << "PASS: " << checks << " custom wake checks (real FFT/VAD/DTW + transactional persistence + capture lifecycle).\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
}
