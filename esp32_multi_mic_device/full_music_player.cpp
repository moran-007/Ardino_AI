#include "full_music_player.h"
#include "music_stream_core.h"
#include "music_voice_commands.h"
#include "voice_input.h"
#include "offline_wake.h"
#include "robot_display.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace FullMusic {
namespace {
constexpr size_t BUFFER_BYTES = 768 * 1024;
constexpr size_t PROXY_RESERVE_BYTES = 48000 * 2 * 6;
constexpr size_t CUES = 100;
struct Cue { uint64_t timeMs; char text[256]; };
class PsramJsonAllocator : public ArduinoJson::Allocator {
 public:
  void *allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *pointer) override { free(pointer); }
  void *reallocate(void *pointer, size_t size) override { return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
PsramJsonAllocator jsonAllocator;
struct Track { char id[96] = {}, title[256] = {}, artist[192] = {}; uint64_t duration = 0; bool lyrics = false; };
SemaphoreHandle_t mutex = nullptr;
TaskHandle_t task = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
MusicStream::Ring ring;
MusicStream::Intent intent;
Cue *cues = nullptr;
size_t cueCount = 0;
int64_t nextCueCursor = -1;
uint64_t nextCueAfter = 0;
char retainedLyric[256] = {};
Track track;
String base, device, credential, playlist, command, argument, error;
const char *certificate = nullptr;
bool workerClosed = true, eof = false, fetching = false, clock48 = false;
bool audioClosed = true, audioBuffering = true;
uint32_t networkHolds = 0, underruns = 0, outputGapMaxMs = 0, lastOutputMs = 0, audioGeneration = 0;
MusicStream::BufferGate playbackGate;
uint32_t lastDisplay = 0;
class Lock {
 public: Lock() { xSemaphoreTake(mutex, portMAX_DELAY); } ~Lock() { xSemaphoreGive(mutex); }
};
void copyText(char *to, size_t size, const char *from) {
  if (!from) from = "";
  size_t n = strlen(from); if (n >= size) n = size - 1;
  // Do not leave a partial UTF-8 code point in a bounded display field.
  while (n && (static_cast<unsigned char>(from[n]) & 0xc0) == 0x80) --n;
  memcpy(to, from, n); to[n] = 0;
}
String encoded(const String &value) {
  String out; const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); ++i) { unsigned char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out += static_cast<char>(c);
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  } return out;
}
String number64(uint64_t value) { char buffer[24]; snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value)); return String(buffer); }
bool allow(uint32_t generation) { Lock lock; return intent.accepts(generation) && intent.downloadAllowed() && !networkHolds; }
void fail(uint32_t generation, const String &message) {
  Lock lock; if (intent.accepts(generation)) { error = message; intent.pause(); fetching = false; }
}
void prepareHttp(HTTPClient &http, WiFiClientSecure &client, const String &url, const String &id, const String &token) {
  client.setCACert(certificate); client.setHandshakeTimeout(2);
  http.setConnectTimeout(1500); http.setTimeout(1000); http.setReuse(false);
  http.useHTTP10(true); http.begin(client, url);
  http.addHeader("X-Device-ID", id); http.addHeader("Authorization", "Bearer " + token);
}
bool jsonGet(const String &path, JsonDocument &doc, uint32_t generation) {
  String url, id, token;
  { Lock lock; url = base + "/voice-api/v1" + path; id = device; token = credential; }
  if (!allow(generation)) return false;
  WiFiClientSecure client; HTTPClient http;
  prepareHttp(http, client, url, id, token);
  const int code = http.GET();
  const int expected = http.getSize();
  // A JSON response is bounded separately from a song. Unknown chunked length
  // cannot bypass the limit because getString is never used on this path.
  String body; body.reserve(8192);
  const uint32_t started = millis();
  if (code == 200 && expected <= 65536) {
    auto &input = http.getStream();
    while (allow(generation) && millis() - started < 4500 && body.length() <= 65536) {
      int count = input.available();
      if (count > 0) { char data[512]; count = min(count, static_cast<int>(sizeof(data)));
        int got = input.read(reinterpret_cast<uint8_t *>(data), count); if (got > 0) body.concat(data, got);
      } else if (!http.connected()) break;
      else delay(2);
      if (expected >= 0 && body.length() == static_cast<size_t>(expected)) break;
    }
  }
  http.end(); client.stop();
  return code == 200 && body.length() <= 65536 &&
      (expected < 0 || body.length() == static_cast<size_t>(expected)) && !deserializeJson(doc, body);
}
Track parseTrack(JsonVariantConst value) {
  Track out; copyText(out.id, sizeof(out.id), value["id"] | "");
  copyText(out.title, sizeof(out.title), value["title"] | "音乐");
  copyText(out.artist, sizeof(out.artist), value["artist"] | "");
  out.duration = value["duration_ms"] | uint64_t{0}; out.lyrics = value["lyrics_available"] | false; return out;
}
bool loadLyrics(const String &id, int64_t cursor, uint32_t generation) {
  JsonDocument doc(&jsonAllocator);
  if (!jsonGet("/music/" + encoded(id) + "/lyrics?limit=100&cursor=" + String(static_cast<unsigned long>(cursor)), doc, generation)) return false;
  Lock lock; if (!intent.accepts(generation)) return false;
  if (cueCount) copyText(retainedLyric, sizeof(retainedLyric), cues[cueCount - 1].text);
  cueCount = 0;
  for (JsonObjectConst item : doc["cues"].as<JsonArrayConst>()) {
    if (cueCount >= CUES) break;
    cues[cueCount].timeMs = item["time_ms"] | uint64_t{0};
    copyText(cues[cueCount].text, sizeof(cues[cueCount].text), item["text"] | ""); ++cueCount;
  }
  nextCueCursor = doc["next_cursor"].isNull() ? -1 : doc["next_cursor"].as<int64_t>();
  // Fetch the next page as the last cue becomes current; hold the last line
  // until the next page arrives, while preserving already queued PCM.
  nextCueAfter = cueCount ? cues[cueCount - 1].timeMs : 0; return true;
}
void networkTask(void *) {
  uint8_t data[4096];
  uint32_t loadedGeneration = 0;
  for (;;) {
    uint32_t generation; String action, arg, list; bool go;
    { Lock lock; workerClosed = true; generation = intent.generation;
      go = intent.downloadAllowed() && !networkHolds;
      if (go) { action = command; arg = argument; list = playlist; workerClosed = false; fetching = true; }
    }
    if (!go) { delay(10); continue; }
    if (action.length()) {
      JsonDocument doc(&jsonAllocator); bool ok = false; Track candidate;
      if (action == "play") {
        ok = jsonGet("/music/" + encoded(arg), doc, generation);
        if (ok) candidate = parseTrack(doc["track"].isNull() ? doc.as<JsonVariantConst>() : doc["track"].as<JsonVariantConst>());
      } else if (action == "search") {
        ok = jsonGet("/music?q=" + encoded(arg) + "&limit=1", doc, generation);
        const int separator = arg.indexOf("的");
        if (ok && !doc["tracks"].size() && separator >= 0 && separator + static_cast<int>(strlen("的")) < static_cast<int>(arg.length()))
          ok = jsonGet("/music?q=" + encoded(arg.substring(separator + strlen("的"))) + "&limit=1", doc, generation);
        if (ok && doc["tracks"].size()) candidate = parseTrack(doc["tracks"][0]);
      } else {
        String current; { Lock lock; current = track.id; }
        ok = jsonGet("/music/queue?playlist_id=" + encoded(list) + "&current=" + encoded(current) + "&direction=" + (action == "previous" ? "previous" : "next"), doc, generation);
        if (ok) candidate = parseTrack(doc["track"]);
      }
      if (!allow(generation)) continue;
      if (!ok || !candidate.id[0]) { fail(generation, "未找到歌曲或云端暂不可用"); continue; }
      { Lock lock; if (!intent.accepts(generation)) continue; track = candidate; command = ""; argument = "";
        cueCount = 0; nextCueCursor = -1; retainedLyric[0] = 0; eof = false; error = ""; }
      loadedGeneration = generation;
      if (candidate.lyrics) loadLyrics(candidate.id, 0, generation);
    } else if (loadedGeneration != generation) { delay(10); continue; }
    uint64_t offset, position, after; int64_t cursor; String trackId, url, id, token;
    { Lock lock; offset = intent.downloadedBytes; position = intent.positionMs(); cursor = nextCueCursor; after = nextCueAfter;
      trackId = track.id; url = base + "/voice-api/v1/music/" + encoded(trackId) + "/pcm?offset_bytes=" + number64(offset);
      id = device; token = credential;
      if (eof) { fetching = false; workerClosed = true; }
    }
    if (cursor >= 0 && position >= after) {
      if (!loadLyrics(trackId, cursor, generation)) { Lock lock; if (intent.accepts(generation)) nextCueCursor = -1; }
    }
    bool ended; { Lock lock; ended = eof; }
    if (ended) { delay(10); continue; }
    if (!allow(generation)) continue;
    WiFiClientSecure client; HTTPClient http; prepareHttp(http, client, url, id, token);
    // HTTP/1.0 + Connection close makes Nginx remove chunk framing; the
    // unknown Content-Length is intentional. Only transport EOF ends a song.
    http.addHeader("Connection", "close");
    const char *keys[] = {"X-Audio-Sample-Rate", "X-Audio-Channels", "X-Audio-Bits", "Transfer-Encoding"}; http.collectHeaders(keys, 4);
    const int code = http.GET();
    if (code != 200 || http.header("Transfer-Encoding").length() ||
        (http.header("X-Audio-Sample-Rate").length() && http.header("X-Audio-Sample-Rate") != "48000") ||
        (http.header("X-Audio-Channels").length() && http.header("X-Audio-Channels") != "1") ||
        (http.header("X-Audio-Bits").length() && http.header("X-Audio-Bits") != "16")) {
      http.end(); client.stop(); fail(generation, "音乐流不可用，HTTP " + String(code)); continue;
    }
    auto &input = http.getStream(); uint32_t lastData = millis(); bool trueEnd = false;
    bool pendingByte = false; uint8_t pending = 0; size_t bytesSinceYield = 0;
    while (allow(generation)) {
      size_t room; bool pageDue;
      { Lock lock; room = ring.space(); pageDue = nextCueCursor >= 0 && intent.positionMs() >= nextCueAfter; }
      if (pageDue) break;
      if (room < 2) { lastData = millis(); delay(3); continue; }
      int available = input.available();
      if (available > 0) {
        const size_t prefix = pendingByte ? 1 : 0;
        if (prefix) data[0] = pending;
        const size_t wanted = min(min(room - prefix, sizeof(data) - prefix), static_cast<size_t>(available));
        const int got = input.read(data + prefix, wanted);
        if (got > 0) { const size_t total = got + prefix, complete = total & ~size_t{1};
          pendingByte = total != complete; if (pendingByte) pending = data[complete];
          { Lock lock; if (intent.accepts(generation)) {
            const size_t saved = ring.write(data, complete); intent.downloadedBytes += saved;
          } }
          lastData = millis(); bytesSinceYield += got;
          // Never sleep while owning the audio ring. Yield in batches rather
          // than on every small TLS fragment; the output task has priority.
          if (bytesSinceYield >= 16384) { bytesSinceYield = 0; delay(1); }
        }
        else delay(2); // SecureClient -1 during TLS record gaps is not EOF.
      } else if (!http.connected()) { trueEnd = true; break; }
      else if (millis() - lastData > 12000) { http.end(); client.stop(); fail(generation, "音乐网络超时，可继续重试"); break; }
      else delay(2);
    }
    http.end(); client.stop();
    { Lock lock; workerClosed = true; fetching = false;
      if (intent.accepts(generation) && trueEnd) {
        const uint64_t downloadedMs = intent.downloadedBytes / 96;
        if (track.duration > 5000 && downloadedMs + 2500 < track.duration) {
          error = "音乐连接提前结束，可继续重试"; intent.pause(); eof = false;
        } else { eof = true; if (pendingByte) { error = "音乐流末尾不是完整 PCM 样本"; intent.pause(); } }
      }
    }
  }
}
bool changeClock(bool music) {
  if (clock48 == music) return true;
  const bool restore = OfflineWake::listenerActive(); OfflineWake::suspend();
  const bool ok = OfflineWake::paused() && VoiceInput::setMusicRate(music);
  if (ok) clock48 = music;
  if (restore) OfflineWake::resume();
  return ok;
}
void enqueue(const String &action, const String &arg) {
  Lock lock; intent.newTrack(); ring.clear(); eof = false; error = ""; command = action; argument = arg;
  audioBuffering = true; underruns = outputGapMaxMs = 0;
}
void audioStep() {
  int16_t samples[480]; size_t bytes = 0; uint32_t generation; bool allowed, held, ended, drain;
  { Lock lock;
    generation = intent.generation; allowed = intent.downloadAllowed(); held = intent.holds != 0; ended = eof;
    if (audioGeneration != generation || !allowed) { playbackGate.reset(); lastOutputMs = 0; audioGeneration = generation; }
    const bool wasReady = playbackGate.ready();
    drain = allowed && playbackGate.update(ring.size(), ended);
    if (allowed && wasReady && !drain && !ended) ++underruns;
    audioBuffering = allowed && !drain;
    // Once a foreground hold is acknowledged, this task must not touch the amp
    // or clocks: TTS/alarms/recording now own that hardware.
    audioClosed = held;
    if (drain) bytes = ring.peek(reinterpret_cast<uint8_t *>(samples), sizeof(samples)) & ~size_t{1};
  }
  if (held) return;
  if (!allowed) {
    if (clock48) { changeClock(false); VoiceInput::muteAmplifier(true); }
  } else if (bytes) {
    if (!changeClock(true)) { fail(generation, "音乐采样率切换失败"); }
    else {
      const uint32_t started = millis();
      VoiceInput::setAmbientTracking(false);
      const size_t written = VoiceInput::playMusicChunk(samples, bytes / 2);
      { Lock lock;
        if (intent.accepts(generation)) {
          if (lastOutputMs) outputGapMaxMs = max(outputGapMaxMs, started - lastOutputMs);
          lastOutputMs = millis(); ring.consume(written * 2); intent.renderedFrames += written;
        }
      }
      if (written != bytes / 2) fail(generation, "音乐输出失败");
    }
  } else if (ended && allowed) { next(); }
  else if (allowed) { VoiceInput::muteAmplifier(true); Lock lock; lastOutputMs = 0; }
  { Lock lock; audioClosed = true; }
}
void audioTask(void *) { for (;;) { audioStep(); delay(1); } }
}
bool begin(const String &url, const String &id, const String &token, const char *ca) {
  if (mutex) { configure(url, id, token); return task != nullptr; }
  mutex = xSemaphoreCreateMutex(); if (!mutex) return false;
  auto *memory = static_cast<uint8_t *>(heap_caps_malloc(BUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  cues = static_cast<Cue *>(heap_caps_calloc(CUES, sizeof(Cue), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!memory || !cues) { free(memory); free(cues); error = "PSRAM 音乐缓存分配失败"; return false; }
  ring.attach(memory, BUFFER_BYTES); certificate = ca; configure(url, id, token);
  if (xTaskCreatePinnedToCore(audioTask, "music-output", 16384, nullptr, 3, &audioTaskHandle, 1) != pdPASS) return false;
  // WakeNet runs on core 0. Keep TLS on core 1, below the PCM output task,
  // so microphone DSP cannot starve downloads or compete with TLS decoding.
  return xTaskCreatePinnedToCore(networkTask, "music-network", 12288, nullptr, 2, &task, 1) == pdPASS;
}
void configure(const String &url, const String &id, const String &token) {
  if (!mutex) return;
  Lock lock;
  if (base == url && device == id && credential == token) return;
  if (base != url || device != id || credential != token) { intent.stop(); ring.clear(); command = ""; }
  base = url; while (base.endsWith("/")) base.remove(base.length() - 1); device = id; credential = token;
}
bool play(const String &id) { if (!task || id.isEmpty() || id.length() > 95) return false; enqueue("play", id); return true; }
bool search(const String &query) {
  if (!task || query.isEmpty()) return false;
  char bounded[241]; copyText(bounded, sizeof(bounded), query.c_str());
  enqueue("search", String(bounded)); return true;
}
void pause() { if (!mutex) return; Lock lock; intent.pause(); }
void resume() { if (!task) return; Lock lock; if (track.id[0] || command.length()) { intent.resume(); error = ""; } }
void stop() { if (!mutex) return; Lock lock; intent.stop(); ring.clear(); command = ""; track = Track(); cueCount = 0; retainedLyric[0] = 0; eof = false; }
void next(bool previous) { if (task) enqueue(previous ? "previous" : "next", ""); }
void selectPlaylist(const String &id) { if (!mutex) return; Lock lock; playlist = id; }
bool active() { if (!mutex) return false; Lock lock; return track.id[0] || command.length(); }
bool playing() { if (!mutex) return false; Lock lock; return intent.playing; }
bool hold(uint32_t timeoutMs) {
  if (!task) return true;
  { Lock lock; intent.hold(); }
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    bool closed; { Lock lock; closed = workerClosed && audioClosed; }
    if (closed) { if (changeClock(false)) return true; break; }
    delay(2);
  }
  release(); return false;
}
void release() { if (!mutex) return; Lock lock; intent.release(); }
String statusJson() {
  JsonDocument doc;
  if (!mutex) { doc["state"] = "unavailable"; } else {
    Lock lock; doc["id"] = track.id; doc["title"] = track.title; doc["artist"] = track.artist;
    doc["duration_ms"] = track.duration; doc["position_ms"] = intent.positionMs(); doc["buffer_bytes"] = ring.size();
    doc["playlist_id"] = playlist; doc["volume"] = VoiceInput::playbackVolume(); doc["error"] = error;
    doc["hardware_sample_rate"] = VoiceInput::hardwareSampleRate();
    doc["tls_closed"] = workerClosed;
    doc["audio_closed"] = audioClosed;
    doc["buffer_ms"] = ring.size() / 96;
    doc["buffer_capacity_bytes"] = BUFFER_BYTES;
    doc["underruns"] = underruns; doc["output_gap_max_ms"] = outputGapMaxMs;
    doc["state"] = error.length() ? "error" : intent.holds ? "suspended" : !intent.playing ? "paused" :
        (command.length() || audioBuffering) ? "buffering" : "playing";
    if (!track.id[0] && !command.length()) doc["state"] = "stopped";
    if (!task) doc["state"] = "unavailable";
    const uint64_t position = intent.positionMs(); const char *current = retainedLyric, *upcoming = "";
    for (size_t i = 0; i < cueCount; ++i) { if (cues[i].timeMs <= position) current = cues[i].text;
      else { upcoming = cues[i].text; break; } }
    doc["lyric"] = current; doc["next_lyric"] = upcoming; doc["generation"] = intent.generation;
  }
  String out; serializeJson(doc, out); return out;
}
void tick() {
  if (!task) return;
  if (millis() - lastDisplay > 250) {
    lastDisplay = millis(); JsonDocument doc; deserializeJson(doc, statusJson());
    const bool visible = doc["state"] != "stopped";
    RobotDisplay::setMusicStatus(visible, String(doc["title"] | "音乐"), String(doc["artist"] | ""),
        String(doc["state"] | ""), String(doc["lyric"] | ""), String(doc["next_lyric"] | ""),
        doc["position_ms"] | uint64_t{0}, doc["duration_ms"] | uint64_t{0});
  }
}
bool request(const String &path, const String &method, const String &body, String &response, int &code) {
  if (!mutex || !task) { code = 503; response = "{\"error\":\"音乐任务未就绪，其他功能可继续使用\"}"; return false; }
  // A short catalogue connection can coexist with the stream when internal RAM
  // has space for another TLS handshake. Keep streaming to avoid repeated WAN
  // handshakes draining the buffer; fall back to serial TLS under memory pressure.
  bool parallel;
  { Lock lock; parallel = intent.downloadAllowed() && !eof; }
  parallel = parallel && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 40960 &&
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= 12288;
  // A serial catalogue request closes streaming TLS while it uses the connection.
  // Refill first, including on consecutive requests, leaving time for both
  // the query and the stream's next TLS handshake. Never pause music to browse.
  const uint32_t refillStarted = millis();
  while (!parallel) {
    bool reserveReady;
    { Lock lock; reserveReady = !intent.downloadAllowed() || eof || ring.size() >= PROXY_RESERVE_BYTES; }
    if (reserveReady) break;
    if (millis() - refillStarted >= 10000) {
      code = 503; response = "{\"error\":\"音乐缓存正在补充，请稍后重试\"}"; return false;
    }
    delay(2);
  }
  // Serialize TLS connections while the independent output task keeps draining
  // queued music. Browsing the library/playlist is not an audio foreground task.
  struct NetworkGuard {
    bool held;
    explicit NetworkGuard(bool serial) : held(serial) { if (held) { Lock lock; ++networkHolds; } }
    ~NetworkGuard() { if (held) { Lock lock; --networkHolds; } }
  } guard(!parallel);
  bool closed = parallel; const uint32_t waiting = millis();
  while (!closed && millis() - waiting < 5000) {
    { Lock lock; closed = workerClosed; }
    if (closed) break;
    delay(2);
  }
  if (!closed) { code = 503; response = "音乐连接未能及时关闭"; return false; }
  String url, id, token; { Lock lock; url = base + "/voice-api/v1" + path; id = device; token = credential; }
  WiFiClientSecure client; HTTPClient http; prepareHttp(http, client, url, id, token); http.setTimeout(3500);
  http.addHeader("Content-Type", "application/json");
  code = method == "GET" ? http.GET() : method == "POST" ? http.POST(body) : http.sendRequest(method.c_str(), body);
  const int expected = http.getSize();
  response = ""; const uint32_t started = millis(); auto &input = http.getStream();
  while (code > 0 && expected <= 65536 && millis() - started < 5000 && response.length() <= 65536) {
    int count = input.available(); if (count > 0) { char data[512]; count = min(count, static_cast<int>(sizeof(data)));
      int got = input.read(reinterpret_cast<uint8_t *>(data), count); if (got > 0) response.concat(data, got);
    } else if (!http.connected()) break; else delay(2);
    if (expected >= 0 && response.length() == static_cast<size_t>(expected)) break;
  }
  http.end(); client.stop(); if (response.length() > 65536) { response = "云端响应过大，请缩小分页"; code = 502; }
  if (code <= 0) response = "{\"error\":\"音乐服务器连接失败，请重试\"}";
  if (code > 0 && expected >= 0 && response.length() != static_cast<size_t>(expected)) { response = "云端响应未完整接收，请重试"; code = 502; }
  return code >= 200 && code < 300;
}
bool handleVoice(const String &text, String &message, bool *volumeChanged) {
  if (volumeChanged) *volumeChanged = false;
  if (text.indexOf("闹钟") >= 0 || text.indexOf("铃声") >= 0 || text.indexOf("提醒") >= 0 ||
      text.indexOf("便签") >= 0 || text.indexOf("待办") >= 0 || text.indexOf("计时") >= 0 || text.indexOf("番茄") >= 0) return false;
  if (text.indexOf("音量") >= 0) {
    const int current = VoiceInput::potentiometerVolumeEnabled() ? VoiceInput::readPotentiometerVolume() : VoiceInput::playbackVolume();
    if (text.indexOf("多少") >= 0 || text.indexOf("查看") >= 0) { message = "当前音量为 " + String(current) + "%。"; return true; }
    const int volume = MusicVoice::volume(text, current);
    if (volume < 0) { message = "请说明音量百分比（0 到 100），或说调大/调小音量。"; return true; }
    VoiceInput::usePotentiometerVolume(false); VoiceInput::setPlaybackVolume(volume);
    if (volumeChanged) *volumeChanged = true;
    message = "音量已调到 " + String(volume) + "%。"; return true;
  }
  if (text.indexOf("上一首") >= 0 || text.indexOf("上一曲") >= 0) { next(true); message = "正在切换上一首。"; return true; }
  if (text.indexOf("下一首") >= 0 || text.indexOf("下一曲") >= 0) { next(); message = "正在切换下一首。"; return true; }
  if (text.indexOf("暂停音乐") >= 0 || text.indexOf("暂停播放") >= 0 || text == "暂停") { if (!active()) { message = "当前没有播放中的音乐。"; return true; } pause(); message = "音乐已暂停。"; return true; }
  if (text.indexOf("继续音乐") >= 0 || text.indexOf("继续播放") >= 0 || text.indexOf("恢复播放") >= 0) { if (!active()) { message = "当前没有可以继续的歌曲。"; return true; } resume(); message = "继续播放音乐。"; return true; }
  if (text.indexOf("停止音乐") >= 0 || text.indexOf("停止播放") >= 0 || text.indexOf("关闭音乐") >= 0) { stop(); message = "音乐已停止。"; return true; }
  int start = text.indexOf("播放");
  if (start >= 0 && text.indexOf("闹钟") < 0 && text.indexOf("铃声") < 0 && text.indexOf("照片") < 0) {
    String query = text.substring(start + strlen("播放")); query.trim();
    while (query.endsWith("。") || query.endsWith("！") || query.endsWith("!")) query.remove(query.length() - (query.endsWith("!") ? 1 : 3));
    if (query.endsWith("的歌曲")) query.remove(query.length() - strlen("的歌曲"));
    else if (query.endsWith("的歌")) query.remove(query.length() - strlen("的歌"));
    if (query == "音乐" || query.isEmpty()) { if (active()) resume(); else next(); }
    else { if (query.startsWith("音乐")) query.remove(0, strlen("音乐")); if (query.startsWith("歌曲")) query.remove(0, strlen("歌曲")); query.trim(); search(query); }
    message = query.isEmpty() || query == "音乐" ? "开始播放音乐。" : "正在查找并播放“" + query + "”。"; return true;
  }
  return false;
}
}
