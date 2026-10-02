// Reproduction against the pre-fix driver; see VERIFICATION.md for observed red.
#include "voice_input.h"
#include "robot_controls.h"
#include "fake_hardware.h"

#include <iostream>

namespace RobotControls {
void begin() {}
bool talkPressed() { return FakeHardware::talkDown && FakeHardware::inputSamples < FakeHardware::releaseAfterSamples; }
bool abortRequested() { return FakeHardware::abortNow || FakeHardware::inputSamples >= FakeHardware::abortAfterSamples; }
bool consumeAbortRequest() { return abortRequested(); }
void clearAbortRequest() { FakeHardware::abortNow = false; }
}

int main() {
  FakeHardware::sampleAt = [](size_t n) -> int16_t { return n % 160 == 0 ? 60 : n % 2 ? -20 : 20; };
  VoiceInput::AudioStats stats;
  const bool recorded = VoiceInput::recordPcmHandsFree("/noise.pcm", 400, 10000, 1200, stats);
  const bool legacyUpload = recorded && stats.rms >= 80 && stats.peak >= 250;
  std::cout << "noise: recorded=" << recorded << " samples=" << stats.samples
            << " rms=" << stats.rms << " peak=" << stats.peak
            << " fileBytes=" << stats.fileBytes << " legacyUpload=" << legacyUpload << '\n';
  if (legacyUpload) {
    std::cerr << "FAIL no-speech input must not pass the upload gate\n";
    return 1;
  }
  std::cout << "PASS no-speech input blocked\n";
  return 0;
}
