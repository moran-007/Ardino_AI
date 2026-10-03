#pragma once
#include <Arduino.h>
namespace FullMusic {
bool begin(const String &baseUrl, const String &deviceId, const String &token, const char *ca);
void configure(const String &baseUrl, const String &deviceId, const String &token);
bool play(const String &id);
bool search(const String &query);
void pause();
void resume();
void stop();
void next(bool previous = false);
void selectPlaylist(const String &id);
bool active();
bool playing();
void tick();
String statusJson();
// Ack includes TLS closure, then the shared audio clocks return to 16 kHz.
bool hold(uint32_t timeoutMs = 5000);
void release();
class Foreground {
 public:
  Foreground() : held_(hold()) {}
  ~Foreground() { if (held_) release(); }
  bool ready() const { return held_; }
 private: bool held_;
};
// Authenticated short cloud proxy; retains queued PCM and user playback intent.
bool request(const String &path, const String &method, const String &body, String &response, int &code);
bool handleVoice(const String &text, String &message, bool *volumeChanged = nullptr);
}
