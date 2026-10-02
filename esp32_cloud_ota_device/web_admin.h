#pragma once

#include <WebServer.h>
#include <string.h>

namespace WebAdmin {
void begin(WebServer &server);
bool authenticated(WebServer &server);
bool requireSession(WebServer &server, bool redirect = false);
void login(WebServer &server);
void logout(WebServer &server);
void invalidateAll();

// Stream fragments instead of allocating a 28+ KB HTML String in internal RAM.
class PageStream {
 public:
  explicit PageStream(WebServer &server) : server_(server) {
    server_.sendHeader("Cache-Control", "no-store");
    server_.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server_.send(200, "text/html; charset=utf-8", "");
  }
  void operator=(const __FlashStringHelper *text) { *this += text; }
  void operator+=(const __FlashStringHelper *text) {
    server_.sendContent_P(reinterpret_cast<PGM_P>(text));
  }
  void operator+=(const String &text) { server_.sendContent(text); }
  void operator+=(const char *text) { server_.sendContent(text, strlen(text)); }
  void finish() { server_.sendContent(""); }
 private:
  WebServer &server_;
};
}  // namespace WebAdmin
