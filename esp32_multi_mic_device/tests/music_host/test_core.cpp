#include "music_stream_core.h"
#include "music_resampler.h"
#include <iostream>
#include <vector>
#include <random>
#include <deque>
#include <stdexcept>
#include <cmath>
size_t checks = 0;
void require(bool ok, const char *message) { ++checks; if (!ok) throw std::runtime_error(message); }
double rms(double hz) {
  MusicStream::Decimator filter; double sum = 0; size_t count = 0;
  for (size_t i = 0; i < 48000; ++i) {
    float value;
    if (filter.push(static_cast<float>(sin(i * 2 * 3.141592653589793 * hz / 48000)), value) && i > 1000) { sum += value * value; ++count; }
  } return sqrt(sum / count);
}
int main() { try {
  uint8_t storage[192 * 1024]; MusicStream::Ring ring; ring.attach(storage, sizeof(storage));
  std::mt19937 rng(456); std::deque<uint8_t> expected;
  for (int step = 0; step < 4000; ++step) {
    size_t count = rng() % 8193; std::vector<uint8_t> bytes(count);
    if (rng() & 1) { for (auto &v : bytes) v = rng();
      const size_t room = sizeof(storage) - expected.size(), written = ring.write(bytes.data(), count);
      require(written == std::min(room, count), "ring did not bound producer");
      expected.insert(expected.end(), bytes.begin(), bytes.begin() + written);
    } else { const size_t got = ring.peek(bytes.data(), count);
      require(got == std::min(expected.size(), count), "ring did not bound consumer");
      for (size_t i = 0; i < got; ++i) require(bytes[i] == expected[i], "ring wrap reordered PCM");
      const size_t accepted = got / 2; ring.consume(accepted); for (size_t i = 0; i < accepted; ++i) expected.pop_front();
    } require(ring.size() == expected.size(), "ring lost partial TX samples");
  }
  ring.clear(); require(ring.size() == 0 && ring.space() == sizeof(storage), "generation reset kept old audio");
  MusicStream::Intent intent; intent.newTrack(); const auto old = intent.generation;
  intent.hold(); intent.pause(); intent.release(); require(!intent.downloadAllowed(), "foreground resume overrode user pause");
  intent.resume(); intent.hold(); intent.hold(); intent.release(); require(!intent.downloadAllowed(), "nested hold released early");
  intent.stop(); intent.release(); require(!intent.downloadAllowed() && !intent.accepts(old), "old download accepted after stop");
  intent.newTrack(); intent.downloadedBytes = (1ULL << 33) + 2; intent.renderedFrames = 48000ULL * 3600 * 100;
  require(intent.positionMs() == 360000000ULL && intent.downloadedBytes > UINT32_MAX, "64-bit progress overflow");
  require(rms(1000) > .69 && rms(5000) > .68, "FIR attenuated speech passband");
  require(rms(10000) < .003 && rms(18000) < .003, "48 kHz microphone aliases into WakeNet");
  MusicStream::Decimator a, b; std::vector<float> continuous, blocked;
  for (int i = 0; i < 4800; ++i) { float v; if (a.push(sin(i * .1), v)) continuous.push_back(v); }
  for (int start = 0; start < 4800; start += 127) for (int i = start; i < std::min(start + 127, 4800); ++i) { float v; if (b.push(sin(i * .1), v)) blocked.push_back(v); }
  require(continuous == blocked && blocked.size() == 1600, "FIR block boundary changed phase");
  b.reset(); for (int i = 0; i < 240; ++i) { float v; if (b.push(0, v)) require(v == 0, "mic/rate reset retained FIR history"); }
  std::cout << "PASS: " << checks << " production ring/intent/anti-alias checks\n"; return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; } }
