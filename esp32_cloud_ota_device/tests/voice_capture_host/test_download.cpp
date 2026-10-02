#include "bounded_audio_download.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
int checks = 0;
void require(bool ok, const char *message) { ++checks; if (!ok) throw std::runtime_error(message); }
struct TlsInput {
  std::vector<uint8_t> bytes;
  size_t position = 0, chunk = 7, shortRead = 3;
  uint32_t readyAt = 0;
  int calls = 0, unbufferedReads = 0;
  bool zeroFirst = false;
  int available() { return millis() >= readyAt ? std::min(chunk, bytes.size() - position) : 0; }
  size_t readBytes(char *target, size_t requested) {
    ++calls;
    const auto buffered = available();
    if (!buffered) { ++unbufferedReads; return 0; }  // SecureClient -1/EOF gap.
    require(requested <= static_cast<size_t>(buffered), "read crossed TLS buffered boundary");
    if (zeroFirst) { zeroFirst = false; readyAt = millis() + 4; return 0; }
    const size_t count = std::min(requested, shortRead);
    std::memcpy(target, bytes.data() + position, count);
    position += count;
    readyAt = millis() + 4;
    return count;
  }
};
}
int main() {
  try {
    fakeMillis = 1000;
    TlsInput input;
    for (int i = 0; i < 101; ++i) input.bytes.push_back(i);
    std::array<uint8_t, 103> destination{};
    destination[101] = 0xa5; destination[102] = 0x5a;
    input.zeroFirst = true;
    require(AudioDownload::readBounded(input, destination.data(), 101, 1000, [] { return false; }) == 101,
            "temporary TLS gap or short read truncated download");
    require(std::equal(input.bytes.begin(), input.bytes.end(), destination.begin()), "partial reads lost PCM bytes");
    require(input.unbufferedReads == 0 && destination[101] == 0xa5 && destination[102] == 0x5a,
            "read outside availability or output bounds");
    TlsInput stalled; stalled.bytes = {1, 2, 3}; stalled.readyAt = millis() + 1000;
    const uint32_t began = millis();
    require(AudioDownload::readBounded(stalled, destination.data(), 3, 20, [] { return false; }) == 0 &&
            millis() - began == 20, "stalled connection ignored download deadline");
    TlsInput partial; partial.bytes = {1, 2, 3, 4}; partial.chunk = partial.shortRead = 2;
    require(AudioDownload::readBounded(partial, destination.data(), 4, 2, [] { return false; }) == 2,
            "deadline erased accurate partial download count");
    TlsInput cancelled; cancelled.bytes = {1, 2, 3, 4}; cancelled.chunk = cancelled.shortRead = 2;
    require(AudioDownload::readBounded(cancelled, destination.data(), 4, 100,
              [&] { return cancelled.position >= 2; }) == 2, "cancel read past requested stop");
    fakeMillis = UINT32_MAX - 3;
    TlsInput wrap; wrap.bytes = {1}; wrap.readyAt = UINT32_MAX;
    require(AudioDownload::readBounded(wrap, destination.data(), 1, 10, [] { return false; }) == 0 &&
            fakeMillis == 6, "deadline failed across millis wrap");
    require(AudioDownload::readBounded(wrap, nullptr, 1, 100, [] { return false; }) == 0,
            "null output accepted");
    std::cout << "PASS: " << checks << " production bounded TLS download checks.\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
