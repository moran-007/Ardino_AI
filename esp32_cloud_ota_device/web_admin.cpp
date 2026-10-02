#include "web_admin.h"

#include <esp_random.h>

namespace WebAdmin {
namespace {
constexpr uint32_t IDLE_TIMEOUT_MS = 8UL * 60UL * 60UL * 1000UL;
constexpr char COOKIE_NAME[] = "robot_session=";
struct Session { char token[33] = {}; uint32_t lastUsed = 0; };
Session sessions[4];

String cookieToken(WebServer &server) {
  const String cookies = server.header("Cookie");
  int start = 0;
  while (start < static_cast<int>(cookies.length())) {
    int end = cookies.indexOf(';', start);
    if (end < 0) end = cookies.length();
    String item = cookies.substring(start, end);
    item.trim();
    if (item.startsWith(COOKIE_NAME)) return item.substring(sizeof(COOKIE_NAME) - 1);
    start = end + 1;
  }
  return "";
}

Session *findSession(WebServer &server) {
  const String token = cookieToken(server);
  if (token.length() != 32) return nullptr;
  for (auto &session : sessions) {
    if (!session.token[0]) continue;
    if (millis() - session.lastUsed >= IDLE_TIMEOUT_MS) {
      session = Session{};
      continue;
    }
    uint8_t difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= token[i] ^ session.token[i];
    if (difference == 0) return &session;
  }
  return nullptr;
}
}  // namespace

void begin(WebServer &server) {
  const char *headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);
}

bool authenticated(WebServer &server) {
  Session *session = findSession(server);
  if (!session) return false;
  session->lastUsed = millis();
  return true;
}

bool requireSession(WebServer &server, bool redirect) {
  if (authenticated(server)) return true;
  server.sendHeader("Cache-Control", "no-store");
  if (redirect) {
    server.sendHeader("Location", "/login");
    server.send(303, "text/plain; charset=utf-8", "请先登录设备后台。");
  } else {
    server.send(401, "text/html; charset=utf-8",
        "<!doctype html><meta charset='utf-8'><p>登录已失效，请<a href='/login'>重新登录</a>。</p>");
  }
  return false;
}

void login(WebServer &server) {
  Session *slot = &sessions[0];
  for (auto &session : sessions) {
    if (!session.token[0]) { slot = &session; break; }
    if (millis() - session.lastUsed > millis() - slot->lastUsed) slot = &session;
  }
  uint8_t random[16];
  esp_fill_random(random, sizeof(random));
  constexpr char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < sizeof(random); ++i) {
    slot->token[i * 2] = hex[random[i] >> 4];
    slot->token[i * 2 + 1] = hex[random[i] & 15];
  }
  slot->token[32] = 0;
  slot->lastUsed = millis();
  server.sendHeader("Set-Cookie", String(COOKIE_NAME) + slot->token +
                    "; Path=/; HttpOnly; SameSite=Strict");
}

void logout(WebServer &server) {
  Session *session = findSession(server);
  if (session) *session = Session{};
  server.sendHeader("Set-Cookie", String(COOKIE_NAME) +
                    "; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
}

void invalidateAll() { for (auto &session : sessions) session = Session{}; }
}  // namespace WebAdmin
