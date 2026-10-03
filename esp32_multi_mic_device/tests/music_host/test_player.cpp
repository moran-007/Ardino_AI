#include <cassert>
#include <iostream>
#include <stdexcept>
#include "../../full_music_player.cpp"

namespace VoiceInput {
uint32_t rate = 16000; size_t outputFrames = 0; bool amplifierMuted = true, pot = true; uint8_t volume = 35;
uint32_t hardwareSampleRate() { return rate; }
bool setMusicRate(bool enabled) { rate = enabled ? 48000 : 16000; return true; }
size_t playMusicChunk(const int16_t *, size_t n) { assert(rate == 48000 && n <= 480); outputFrames += n; return n; }
void setAmbientTracking(bool) {} void muteAmplifier(bool value) { amplifierMuted = value; }
uint8_t playbackVolume() { return volume; }
void setPlaybackVolume(uint8_t value) { volume = value; }
void usePotentiometerVolume(bool enabled) { pot = enabled; }
bool potentiometerVolumeEnabled() { return pot; }
uint8_t readPotentiometerVolume() { return volume; }
}
namespace OfflineWake { bool isPaused = false; bool listening() { return false; } bool listenerActive() { return !isPaused; } void suspend() { isPaused = true; } bool paused() { return isPaused; } void resume() { isPaused = false; } }
namespace RobotDisplay { void setMusicStatus(bool, const String &, const String &, const String &, const String &, const String &, uint64_t, uint64_t) {} }
size_t checks = 0;
void require(bool ok, const char *msg) { ++checks; if (!ok) throw std::runtime_error(msg); }
struct StopWorker {};
void runWorkerUntilClosed() {
  fakeDelayHook = []() { if (FullMusic::workerClosed && (FullMusic::eof || !FullMusic::intent.playing)) throw StopWorker(); };
  try { FullMusic::networkTask(nullptr); } catch (const StopWorker &) {}
  fakeDelayHook = {};
}
int main() { try {
  require(FullMusic::begin("https://test", "device", "secret", "CA"), "production player startup failed");
  FullMusic::play("track"); const auto oldGeneration = FullMusic::intent.generation;
  require(FullMusic::intent.playing, "play command did not queue intent");
  FullMusic::workerClosed = true;
  require(FullMusic::hold(), "foreground hold failed");
  FullMusic::pause(); FullMusic::release(); require(!FullMusic::playing(), "release overrode pause command");
  FullMusic::resume(); FullMusic::stop(); require(!FullMusic::intent.accepts(oldGeneration) && !FullMusic::active(), "stop retained track or accepted old packet");
  String message;
  require(FullMusic::handleVoice("继续播放音乐", message) && message.indexOf("没有") >= 0, "empty resume claimed success");
  require(!FullMusic::handleVoice("添加便签明天播放强军战歌", message), "music stole memo command");
  require(!FullMusic::handleVoice("停止闹钟", message), "music stole alarm command");
  bool changed = false;
  require(FullMusic::handleVoice("音量调到百分之三十", message, &changed) && changed && VoiceInput::volume == 30 && !VoiceInput::pot, "Chinese volume command not applied");
  require(FullMusic::handleVoice("调大音量", message, &changed) && changed && VoiceInput::volume == 40, "relative louder command not applied");
  require(FullMusic::handleVoice("调小音量", message, &changed) && changed && VoiceInput::volume == 30, "relative quieter command not applied");
  require(FullMusic::handleVoice("音量调到200", message, &changed) && VoiceInput::volume == 100, "absolute volume not bounded");
  require(FullMusic::handleVoice("音量调到0", message, &changed) && VoiceInput::volume == 0, "voice mute not applied");
  require(FullMusic::handleVoice("音量是多少", message, &changed) && !changed, "volume query changed settings");
  require(!FullMusic::handleVoice("添加便签明天音量调到30", message, &changed) && !changed, "volume stole memo content");
  require(FullMusic::handleVoice("播放阎维文的歌曲。", message) && FullMusic::argument == "阎维文", "artist speech query normalization failed");
  JsonDocument doc;
  require(FullMusic::jsonGet("/music/track", doc, FullMusic::intent.generation), "TLS gaps/negative reads truncated metadata");
  FakeNetwork::truncateJson = true;
  require(!FullMusic::jsonGet("/music/track", doc, FullMusic::intent.generation), "truncated known JSON accepted");
  FakeNetwork::truncateJson = false;
  require(FakeNetwork::active == 0, "metadata TLS remained open");
  FullMusic::track = FullMusic::parseTrack(doc.as<JsonVariantConst>());
  FullMusic::track.duration = 100000; FullMusic::intent.renderedFrames = 48000 * 10;
  FullMusic::cueCount = 2; FullMusic::cues[0].timeMs = 0; strcpy(FullMusic::cues[0].text, "第一句");
  FullMusic::cues[1].timeMs = 11000; strcpy(FullMusic::cues[1].text, "下一句");
  JsonDocument status; deserializeJson(status, FullMusic::statusJson());
  require(status["position_ms"] == 10000 && status["lyric"] == "第一句" && status["next_lyric"] == "下一句", "status lyrics not derived from rendered frames");
  require(status["hardware_sample_rate"] == 16000 && status["tls_closed"] == true, "hardware/TLS diagnostics missing");
  // Exercise real tick with PCM buffered but never whole-song allocation.
  FullMusic::command = ""; FullMusic::intent.resume(); FullMusic::ring.clear();
  uint8_t bytes[2048] = {}; FullMusic::ring.write(bytes, sizeof(bytes)); const auto before = FullMusic::intent.renderedFrames;
  FullMusic::eof = true; FullMusic::audioStep(); require(!OfflineWake::isPaused && VoiceInput::rate == 48000 && FullMusic::intent.renderedFrames == before + 480 && FullMusic::ring.size() == 1088, "bounded tick/progress/consume failed");
  String metadata; int metadataCode;
  require(FullMusic::request("/music", "GET", "", metadata, metadataCode) && VoiceInput::rate == 48000 && FullMusic::intent.holds == 0,
      "music browsing paused output or changed audio clock");
  fakeFreeInternal = 40000;
  FullMusic::eof = false; FullMusic::ring.clear(); bool refillObserved = false;
  fakeDelayHook = [&]() {
    if (refillObserved) return;
    require(FullMusic::networkHolds == 0 && FullMusic::intent.holds == 0,
        "catalogue refill prevented stream/audio progress");
    std::vector<uint8_t> reserve(FullMusic::PROXY_RESERVE_BYTES);
    FullMusic::ring.write(reserve.data(), reserve.size()); refillObserved = true;
  };
  require(FullMusic::request("/music", "GET", "", metadata, metadataCode) && refillObserved,
      "catalogue did not refill before taking streaming TLS");
  fakeDelayHook = {}; FullMusic::ring.clear(); const auto requestCount = FakeNetwork::requests;
  require(!FullMusic::request("/music", "GET", "", metadata, metadataCode) && metadataCode == 503 &&
      FakeNetwork::requests == requestCount && FullMusic::intent.playing && !FullMusic::networkHolds,
      "refill timeout interrupted music or opened metadata TLS");
  fakeFreeInternal = 100000; fakeLargestInternal = 8000;
  require(!FullMusic::request("/music", "GET", "", metadata, metadataCode) && metadataCode == 503 &&
      FakeNetwork::requests == requestCount && !FullMusic::networkHolds,
      "fragmented internal heap allowed another TLS handshake");
  fakeLargestInternal = 32768; FullMusic::workerClosed = false;
  require(FullMusic::request("/music", "GET", "", metadata, metadataCode) && !FullMusic::networkHolds &&
      FullMusic::intent.holds == 0 && FullMusic::intent.playing,
      "RAM-safe metadata connection cancelled the ongoing music stream");
  FullMusic::workerClosed = true;
  FullMusic::eof = true;
  require(FullMusic::hold() && VoiceInput::rate == 16000 && FakeNetwork::active == 0, "foreground did not close TLS and restore rate");
  VoiceInput::amplifierMuted = false; FullMusic::audioStep();
  require(!VoiceInput::amplifierMuted, "background output muted foreground TTS");
  FullMusic::pause(); FullMusic::release(); FullMusic::audioStep(); require(VoiceInput::rate == 16000 && !FullMusic::playing(), "paused user intent resumed after foreground");
  require(FakeNetwork::requests >= 2 && FakeNetwork::reads > FakeNetwork::requests, "metadata test did not exercise partial reads");
  FakeNetwork::audioBytes = 12344; FakeNetwork::durationMs = 100;
  FullMusic::play("track"); runWorkerUntilClosed();
  require(FullMusic::eof && FullMusic::intent.downloadedBytes == 12344 && FullMusic::ring.size() == 12344 && FakeNetwork::active == 0,
      "unknown length PCM/odd TLS records did not reach true EOF");
  std::vector<uint8_t> received(12344); FullMusic::ring.peek(received.data(), received.size());
  for (size_t i = 0; i < received.size(); ++i) require(received[i] == static_cast<uint8_t>(i & 255), "TLS record boundary corrupted PCM");
  FakeNetwork::audioBytes = 12345; FullMusic::play("track"); runWorkerUntilClosed();
  require(!FullMusic::playing() && FullMusic::error.indexOf("完整") >= 0 && FullMusic::intent.downloadedBytes == 12344,
      "odd PCM EOF not rejected or resume offset odd");
  FakeNetwork::audioBytes = 12344; FakeNetwork::durationMs = 100000; FullMusic::play("track"); runWorkerUntilClosed();
  require(!FullMusic::eof && !FullMusic::playing() && FullMusic::error.indexOf("提前") >= 0, "truncated stream silently skipped remainder");
  FakeNetwork::durationMs = 100; FakeNetwork::cancelAt = 1500; FakeNetwork::cancel = []() { FullMusic::stop(); };
  FullMusic::play("track"); runWorkerUntilClosed();
  require(FullMusic::ring.size() == 0 && FullMusic::intent.downloadedBytes == 0 && FakeNetwork::active == 0, "cancel accepted stale read or left TLS open");
  FakeNetwork::cancelAt = SIZE_MAX; FakeNetwork::audioBytes = 1000000; FullMusic::play("track");
  bool filled = false;
  fakeDelayHook = [&]() {
    if (FullMusic::ring.size() == FullMusic::BUFFER_BYTES) { filled = true; FullMusic::stop(); }
    if (filled && FullMusic::workerClosed) throw StopWorker();
  };
  try { FullMusic::networkTask(nullptr); } catch (const StopWorker &) {} fakeDelayHook = {};
  require(filled && FakeNetwork::active == 0, "large unknown stream did not bound memory or cancel while full");
  FullMusic::task = nullptr; String response; int code;
  require(!FullMusic::request("/music", "GET", "", response, code) && code == 503,
      "unavailable music task attempted proxy/invalid mutex");
  std::cout << "PASS: " << checks << " production player API/JSON/TLS/status/tick checks\n"; return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; } }
