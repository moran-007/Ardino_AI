#include "Arduino.h"
#include "esp_partition.h"
#include "esp_wn_iface.h"
#include "../../custom_wake.h"
#include "../../offline_wake.h"
#include <vector>
#include <iostream>
#include <stdexcept>

namespace {
size_t checks = 0, position = 0, officialCalls = 0;
wakenet_state_t officialResult = WAKENET_NO_DETECT;
std::vector<int16_t> pcm;
void require(bool value, const char *error) { ++checks;if (!value) throw std::runtime_error(error); }
std::vector<int16_t> sample() {
  std::vector<int16_t> out(48000);
  for (size_t i = 1600; i < 16000; ++i) {
    const float hz = i < 6200 ? 450 : i < 11200 ? 1100 : 2700;
    out[i] = static_cast<int16_t>(6000 * sinf(6.2831853f * hz * i / 16000));
  }
  return out;
}
model_iface_data_t *create(const char *, int) { static model_iface_data_t model;return &model; }
void destroy(model_iface_data_t *) {}
int rate(model_iface_data_t *) { return 16000; }
int channels(model_iface_data_t *) { return 1; }
int chunks(model_iface_data_t *) { return 512; }
wakenet_state_t detect(model_iface_data_t *, const int16_t *) { ++officialCalls;return officialResult; }
void runListener() {
  stopWakeTaskAtSleep = true;
  try { wakeTaskFunction(nullptr); } catch (const std::runtime_error &) {}
  stopWakeTaskAtSleep = false;
}
}
extern "C" const esp_wn_iface_t esp_sr_wakenet9_quantized = {create, destroy, rate, channels, chunks, detect};
namespace VoiceInput {
bool ready() { return true; }
bool discardMicrophone(uint32_t) { return true; }
bool readMicrophoneMono16(int16_t *samples, size_t count, uint32_t) {
  if (position + count > pcm.size()) return false;
  memcpy(samples, pcm.data() + position, count * 2);position += count;fakeMillis += count * 1000 / 16000;return true;
}
}
int main(int argc, char **) {
  try {
    fs::FS storage;officialPartitionPresent = argc > 1;
    require(CustomWake::begin(storage), "custom storage must initialize");
    require(OfflineWake::begin(true, OfflineWake::DEFAULT_MODEL) == officialPartitionPresent,
            "official missing partition must remain an explicit error");
    require(OfflineWake::modelOptionCount() == 13, "official model package must remain unchanged");
    require(wakeTaskFunction != nullptr, "listener must exist for custom fallback even without WakeNet models");
    if (!officialPartitionPresent) require(!OfflineWake::ready(), "missing official model must not pretend ready before enrollment");
    pcm = sample();position = 0;OfflineWake::suspend();
    require(CustomWake::setLabel("任意词语"), "arbitrary custom phrase must save");
    require(CustomWake::queueTraining() && CustomWake::processQueuedTraining(), "foreground audio template must enroll");
    require(CustomWake::enable(true), "custom enrollment must enable");
    require(OfflineWake::ready(), "trained custom wake must be ready with missing official partition");
    OfflineWake::resume();pcm = sample();position = 0;runListener();
    require(OfflineWake::consumeDetected(), "real offline listener must dispatch custom acoustic detection");
    require(!OfflineWake::consumeDetected(), "wake event must be consumed exactly once");
    require(OfflineWake::paused() && !OfflineWake::listening(), "detection must pause microphone ownership before capture");
    require(String(OfflineWake::wakeWord()) == "任意词语", "active label must describe custom acoustic template");
    require(officialCalls == 0, "custom mode must not call WakeNet with 320-sample custom frames");
    require(CustomWake::enable(false), "custom mode must turn off");
    OfflineWake::resume();pcm = sample();position = 0;runListener();
    if (officialPartitionPresent) require(officialCalls > 0 && OfflineWake::ready(), "disabling custom must preserve the official model and its chunk format");
    else require(!OfflineWake::ready(), "disabling custom without official model must clear readiness");
    require(!OfflineWake::consumeDetected(), "disabled custom mode must not dispatch template events");
    if (officialPartitionPresent) {
      for (const auto result : {WAKENET_CHANNEL_VERIFIED, WAKENET_NO_DETECT, WAKENET_DETECTED}) {
        OfflineWake::suspend();runListener();OfflineWake::resume();
        pcm = sample();position = 0;officialResult = result;runListener();
        require(OfflineWake::consumeDetected() == (result == WAKENET_DETECTED), "actual listener must trigger only official state +1, never channel verification -1 or no detection 0");
        require(!OfflineWake::consumeDetected(), "official wake event must be consumed exactly once");
      }
    }
    std::cout << "PASS: " << checks << " OfflineWake/custom integration checks, official models " << (officialPartitionPresent ? "present" : "missing") << ".\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
}
