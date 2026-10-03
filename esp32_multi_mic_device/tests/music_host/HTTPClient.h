#pragma once
#include <WiFiClientSecure.h>
#include <vector>
#include <functional>
namespace FakeNetwork {
inline bool truncateJson = false, malformed = false;
inline unsigned requests = 0, reads = 0, gaps = 0;
inline size_t audioBytes = 12345, cancelAt = SIZE_MAX;
inline uint64_t durationMs = 100;
inline std::function<void()> cancel;
inline std::vector<String> urls;
}
class FakeInput {
 public:
  std::vector<uint8_t> data; size_t cursor = 0; bool pcm = false;
  int available() {
    ++FakeNetwork::gaps;
    // TLS records periodically arrive after an available()==0 gap.
    if (FakeNetwork::gaps % 4 == 0 && cursor < data.size()) return 0;
    const size_t end = !pcm && FakeNetwork::truncateJson ? data.size() / 2 : data.size();
    return cursor >= end ? 0 : static_cast<int>(std::min<size_t>(end - cursor, pcm ? 127 : 17));
  }
  int read(uint8_t *out, size_t n) {
    ++FakeNetwork::reads;
    if (FakeNetwork::reads % 7 == 0) return -1;
    n = std::min(n, data.size() - cursor); memcpy(out, data.data() + cursor, n); cursor += n;
    if (pcm && cursor >= FakeNetwork::cancelAt && FakeNetwork::cancel) { auto callback = FakeNetwork::cancel; FakeNetwork::cancel = {}; callback(); }
    return static_cast<int>(n);
  }
};
class HTTPClient {
 public:
  void setConnectTimeout(unsigned) {} void setTimeout(unsigned) {} void setReuse(bool reuse) { assert(!reuse); }
  void useHTTP10(bool on) { assert(on); }
  bool begin(WiFiClientSecure &client, const String &url) { client_ = &client; url_ = url; return true; }
  void addHeader(const String &, const String &) {}
  void collectHeaders(const char **, size_t) {}
  String header(const String &name) { return name == "X-Audio-Sample-Rate" ? "48000" : ""; }
  int GET() {
    ++FakeNetwork::requests; FakeNetwork::urls.push_back(url_); client_->opened = true; ++FakeNetwork::active;
    input_ = FakeInput(); input_.pcm = url_.indexOf("/pcm?") >= 0;
    if (input_.pcm) {
      const int at = url_.indexOf("offset_bytes="); const uint64_t offset = at < 0 ? 0 : strtoull(url_.substring(at + strlen("offset_bytes=")).c_str(), nullptr, 10);
      assert((offset & 1U) == 0);
      if (offset < FakeNetwork::audioBytes) input_.data.resize(FakeNetwork::audioBytes - offset);
      for (size_t i = 0; i < input_.data.size(); ++i) input_.data[i] = static_cast<uint8_t>((offset + i) & 255);
      size_ = -1;
    } else {
      String text = url_.indexOf("/lyrics?") >= 0 ? String("{\"cues\":[{\"time_ms\":0,\"text\":\"中文歌词\"},{\"time_ms\":20,\"text\":\"下一句\"}],\"next_cursor\":null,\"total_cues\":2}") :
        String("{\"id\":\"track\",\"title\":\"歌曲\",\"artist\":\"歌手\",\"duration_ms\":") + String(FakeNetwork::durationMs) + ",\"lyrics_available\":true}";
      input_.data.assign(text.c_str(), text.c_str() + text.length()); size_ = input_.data.size();
    } return 200;
  }
  int POST(const String &) { return GET(); }
  int sendRequest(const char *, const String &) { return GET(); }
  int getSize() { return size_; }
  FakeInput &getStream() { return input_; }
  bool connected() { return input_.cursor < ((!input_.pcm && FakeNetwork::truncateJson) ? input_.data.size() / 2 : input_.data.size()); }
  void end() { if (client_) client_->stop(); }
 private: WiFiClientSecure *client_ = nullptr; String url_; FakeInput input_; int size_ = 0;
};
