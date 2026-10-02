#include "focus_timer.h"
#include <ArduinoJson.h>
#include <Preferences.h>

namespace FocusTimer {
namespace {
constexpr char STORAGE_NAMESPACE[] = "robot-focus";
constexpr char STORAGE_KEY[] = "state";
constexpr uint32_t CHECKPOINT_MS = 60000;
constexpr uint32_t RETRY_MS = 5000;
enum class Phase : uint8_t { Idle, Focus, ShortBreak, LongBreak };
struct State {
  Config config;
  Phase phase = Phase::Idle;
  bool paused = false;
  uint32_t remainingMs = 0;
  uint32_t phaseTotalMs = 0;
  uint32_t completedSessions = 0;
  uint8_t cycleSessions = 0;
  uint32_t anchorMs = 0;
};
State current;
bool ready = false;
bool storageError = false;
bool errorReported = false;
uint32_t lastCheckpointMs = 0;
uint32_t lastAttemptMs = 0;

const char *phaseKey(Phase phase) {
  switch (phase) {
    case Phase::Focus: return "focus";
    case Phase::ShortBreak: return "short_break";
    case Phase::LongBreak: return "long_break";
    default: return "idle";
  }
}
const char *phaseLabel(Phase phase) {
  switch (phase) {
    case Phase::Focus: return "专注";
    case Phase::ShortBreak: return "短休息";
    case Phase::LongBreak: return "长休息";
    default: return "待开始";
  }
}
uint32_t remainingAt(const State &state, uint32_t now) {
  if (state.phase == Phase::Idle || state.paused) return state.remainingMs;
  // Unsigned subtraction preserves elapsed time across the millis rollover.
  const uint32_t elapsed = now - state.anchorMs;
  return elapsed >= state.remainingMs ? 0 : state.remainingMs - elapsed;
}
State snapshot(uint32_t now) {
  State state = current;
  state.remainingMs = remainingAt(state, now);
  state.anchorMs = now;
  return state;
}
void writeConfig(JsonDocument &doc, const Config &config) {
  doc["focus_minutes"] = config.focusMinutes;
  doc["short_break_minutes"] = config.shortBreakMinutes;
  doc["long_break_minutes"] = config.longBreakMinutes;
  doc["rounds_before_long_break"] = config.roundsBeforeLongBreak;
}
bool save(const State &state) {
  JsonDocument doc;
  doc["version"] = 1;
  writeConfig(doc, state.config);
  doc["phase"] = static_cast<uint8_t>(state.phase);
  doc["paused"] = state.paused;
  doc["remaining_ms"] = state.remainingMs;
  doc["phase_total_ms"] = state.phaseTotalMs;
  doc["completed_sessions"] = state.completedSessions;
  doc["cycle_sessions"] = state.cycleSessions;
  String payload;
  if (doc.overflowed() || !serializeJson(doc, payload)) return false;
  Preferences storage;
  if (!storage.begin(STORAGE_NAMESPACE, false)) return false;
  // One NVS value makes each state/configuration update atomic.
  const bool ok = storage.putString(STORAGE_KEY, payload) == payload.length();
  storage.end();
  return ok;
}
bool load(const String &payload, State &state) {
  JsonDocument doc;
  if (payload.length() > 1024 || deserializeJson(doc, payload) || !doc["version"].is<unsigned>() || doc["version"] != 1 ||
      !doc["focus_minutes"].is<uint16_t>() || !doc["short_break_minutes"].is<uint16_t>() ||
      !doc["long_break_minutes"].is<uint16_t>() || !doc["rounds_before_long_break"].is<uint8_t>() ||
      !doc["phase"].is<uint8_t>() || !doc["paused"].is<bool>() ||
      !doc["remaining_ms"].is<uint32_t>() || !doc["phase_total_ms"].is<uint32_t>() ||
      !doc["completed_sessions"].is<uint32_t>() || !doc["cycle_sessions"].is<uint8_t>()) return false;
  state.config.focusMinutes = doc["focus_minutes"];
  state.config.shortBreakMinutes = doc["short_break_minutes"];
  state.config.longBreakMinutes = doc["long_break_minutes"];
  state.config.roundsBeforeLongBreak = doc["rounds_before_long_break"];
  if (!validConfig(state.config) || doc["phase"].as<uint8_t>() > static_cast<uint8_t>(Phase::LongBreak)) return false;
  state.phase = static_cast<Phase>(doc["phase"].as<uint8_t>());
  state.remainingMs = doc["remaining_ms"];
  state.phaseTotalMs = doc["phase_total_ms"];
  state.completedSessions = doc["completed_sessions"];
  state.cycleSessions = doc["cycle_sessions"];
  if (state.cycleSessions > 12 || state.completedSessions < state.cycleSessions ||
      state.remainingMs > state.phaseTotalMs) return false;
  if (state.phase == Phase::Idle) {
    if (state.remainingMs || state.phaseTotalMs || state.cycleSessions) return false;
  } else if (state.phaseTotalMs < 60000 || state.phaseTotalMs > 7200000) return false;
  state.paused = state.phase != Phase::Idle;
  return true;
}
void apply(const State &state, uint32_t now) {
  current = state;
  current.anchorMs = now;
  lastCheckpointMs = now;
  lastAttemptMs = now;
  storageError = false;
  errorReported = false;
}
void beginPhase(State &state, Phase phase) {
  state.phase = phase;
  const uint16_t minutes = phase == Phase::Focus ? state.config.focusMinutes :
                           phase == Phase::LongBreak ? state.config.longBreakMinutes : state.config.shortBreakMinutes;
  state.phaseTotalMs = static_cast<uint32_t>(minutes) * 60000U;
  state.remainingMs = state.phaseTotalMs;
}
void nextPhase(State &state, bool completed) {
  if (state.phase == Phase::Focus) {
    if (completed) {
      if (state.completedSessions < UINT32_MAX) ++state.completedSessions;
      if (state.cycleSessions < 12) ++state.cycleSessions;
    }
    beginPhase(state, state.cycleSessions >= state.config.roundsBeforeLongBreak ? Phase::LongBreak : Phase::ShortBreak);
  } else {
    if (state.phase == Phase::LongBreak) state.cycleSessions = 0;
    beginPhase(state, Phase::Focus);
  }
}
CommandResult reject(const String &message) { return {true, false, message}; }
String countdown(uint32_t milliseconds) {
  const uint32_t seconds = (milliseconds + 999U) / 1000U;
  const uint32_t minutes = seconds / 60U;
  const uint32_t remainder = seconds % 60U;
  return String(minutes) + ":" + (remainder < 10 ? "0" : "") + String(remainder);
}
String compact(String value) {
  value.trim();
  for (const char *token : {" ", "\t", "\r", "\n", "，", "。", "！", "!"}) value.replace(token, "");
  if (value.startsWith("请")) value.remove(0, String("请").length());
  if (value.startsWith("帮我")) value.remove(0, String("帮我").length());
  return value;
}
String chineseNumber(unsigned value) {
  static const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  String output;
  if (value >= 100) {
    output = String(digits[value / 100]) + "百";
    value %= 100;
    if (!value) return output;
    if (value < 10) output += "零";
  }
  if (value >= 10) {
    if (value >= 20 || !output.isEmpty()) output += digits[value / 10];
    output += "十";
    value %= 10;
    if (!value) return output;
  }
  output += digits[value];
  return output;
}
int parseMinutes(String token) {
  if (token == "两") return 2;
  for (unsigned value = 1; value <= 120; ++value) {
    if (token == String(value) || token == chineseNumber(value)) return value;
  }
  return -1;
}
}  // namespace

bool validConfig(const Config &config) {
  return config.focusMinutes >= 1 && config.focusMinutes <= 120 &&
         config.shortBreakMinutes >= 1 && config.shortBreakMinutes <= 60 &&
         config.longBreakMinutes >= 1 && config.longBreakMinutes <= 120 &&
         config.roundsBeforeLongBreak >= 1 && config.roundsBeforeLongBreak <= 12;
}
bool begin() {
  current = State{};
  ready = false;
  storageError = false;
  errorReported = false;
  current.anchorMs = lastCheckpointMs = lastAttemptMs = millis();
  Preferences storage;
  if (!storage.begin(STORAGE_NAMESPACE, false)) { storageError = true; return false; }
  const String payload = storage.getString(STORAGE_KEY, "");
  storage.end();
  State restored;
  if (!payload.isEmpty()) {
    if (!load(payload, restored)) { storageError = true; return false; }
    restored.anchorMs = current.anchorMs;
    current = restored;
  }
  ready = true;
  if (current.phase != Phase::Idle && !save(current)) {
    storageError = true;
    return false;
  }
  return true;
}
Config getConfig() { return current.config; }
bool active() { return current.phase != Phase::Idle; }
bool focusing() { return current.phase == Phase::Focus && !current.paused; }
String displaySummary() {
  if (!active()) return "番茄钟待开始";
  return String(phaseLabel(current.phase)) + (current.paused ? "（已暂停） " : " ") +
         countdown(remainingAt(current, millis())) + "\n已完成 " + String(current.completedSessions) + " 次专注";
}
String toJson() {
  JsonDocument doc;
  writeConfig(doc, current.config);
  doc["initialized"] = ready;
  doc["phase"] = phaseKey(current.phase);
  doc["status"] = !active() ? "idle" : current.paused ? "paused" : "running";
  doc["remaining_seconds"] = (remainingAt(current, millis()) + 999U) / 1000U;
  doc["phase_total_seconds"] = current.phaseTotalMs / 1000U;
  doc["completed_sessions"] = current.completedSessions;
  doc["cycle_sessions"] = current.cycleSessions;
  doc["storage_error"] = storageError;
  String output;
  serializeJson(doc, output);
  return output;
}
CommandResult control(const String &action, const Config *config) {
  if (action == "state") return {true, false, displaySummary()};
  if (action != "start" && action != "pause" && action != "resume" && action != "stop" &&
      action != "skip" && action != "configure") return reject("未知番茄钟操作");
  if (!ready) return reject("番茄钟存储不可用，请检查设备存储后重启");
  if (config && action != "configure" && action != "start") return reject("此操作不能同时修改番茄钟配置");
  if (config && !validConfig(*config)) return reject("专注需 1～120 分钟，短休息需 1～60 分钟，长休息需 1～120 分钟，轮数需 1～12");
  if (action == "configure" && !config) return reject("请提供番茄钟配置");
  State next = snapshot(millis());
  if (config) next.config = *config;
  String message;
  if (action == "start") {
    if (active()) return reject(current.paused ? "已有暂停的番茄钟，请说继续专注或先结束" : "番茄钟已经运行，请先结束后再开始");
    next.cycleSessions = 0;
    next.paused = false;
    beginPhase(next, Phase::Focus);
    message = "开始专注 " + String(next.config.focusMinutes) + " 分钟，完成后会提醒休息";
  } else if (action == "pause") {
    if (!active() || current.paused) return reject(!active() ? "番茄钟尚未开始" : "番茄钟已经暂停");
    next.paused = true;
    message = "已暂停番茄钟，剩余 " + countdown(next.remainingMs);
  } else if (action == "resume") {
    if (!active() || !current.paused) return reject(!active() ? "番茄钟尚未开始" : "番茄钟正在运行");
    next.paused = false;
    message = "继续" + String(phaseLabel(next.phase)) + "，剩余 " + countdown(next.remainingMs);
  } else if (action == "stop") {
    if (!active()) return reject("番茄钟尚未开始");
    next.phase = Phase::Idle;
    next.paused = false;
    next.remainingMs = next.phaseTotalMs = 0;
    next.cycleSessions = 0;
    message = "已结束番茄钟";
  } else if (action == "skip") {
    if (!active()) return reject("番茄钟尚未开始");
    nextPhase(next, false);
    message = "已跳到" + String(phaseLabel(next.phase)) + " " + String(next.phaseTotalMs / 60000U) + " 分钟";
    if (next.paused) message += "，保持暂停";
  } else {
    message = "番茄钟设置已保存";
    if (active()) message += "，新时长从下个阶段生效";
  }
  lastAttemptMs = millis();
  if (!save(next)) { storageError = true; return reject("番茄钟保存失败，操作未生效，请稍后重试"); }
  apply(next, next.anchorMs);
  return {true, true, message};
}
CommandResult handleNaturalLanguage(const String &input) {
  const String text = compact(input);
  for (const char *subject : {"专注", "番茄钟", "专注模式"}) {
    if (text == String("暂停") + subject) return control("pause");
    if (text == String("继续") + subject || text == String("恢复") + subject) return control("resume");
    if (text == String("结束") + subject || text == String("停止") + subject || text == String("关闭") + subject) return control("stop");
    if (text == String(subject) + "状态" || text == String("查询") + subject) return control("state");
    if (text == String("跳过") + subject || text == String("跳过当前") + subject) return control("skip");
  }
  if (text == "跳过休息" || text == "跳过当前阶段") return control("skip");
  for (const char *prefix : {"开始番茄钟", "开始专注模式", "开始专注", "开启番茄钟", "开启专注"}) {
    if (!text.startsWith(prefix)) continue;
    const String duration = text.substring(String(prefix).length());
    if (duration.isEmpty()) return control("start");
    if (!duration.endsWith("分钟")) return reject("请说开始专注 25 分钟，时长需 1～120 分钟");
    const int minutes = parseMinutes(duration.substring(0, duration.length() - String("分钟").length()));
    if (minutes < 1) return reject("专注时长需为 1～120 的整数分钟");
    Config config = current.config;
    config.focusMinutes = static_cast<uint16_t>(minutes);
    return control("start", &config);
  }
  return {};
}
bool poll(String &announcement) {
  announcement = "";
  if (!ready || !active() || current.paused) return false;
  const uint32_t now = millis();
  current = snapshot(now);
  const bool finished = current.remainingMs == 0;
  if (!finished && now - lastCheckpointMs < CHECKPOINT_MS) return false;
  if (storageError && now - lastAttemptMs < RETRY_MS) return false;
  State next = current;
  if (finished) nextPhase(next, true);
  lastAttemptMs = now;
  if (!save(next)) {
    storageError = true;
    if (errorReported) return false;
    errorReported = true;
    announcement = "番茄钟保存失败，正在重试，请检查设备存储";
    return true;
  }
  apply(next, now);
  if (!finished) return false;
  announcement = next.phase == Phase::Focus ?
    "休息结束，开始下一轮专注 " + String(next.config.focusMinutes) + " 分钟" :
    "本轮专注完成，已完成 " + String(next.completedSessions) + " 次，开始" +
      String(phaseLabel(next.phase)) + " " + String(next.phaseTotalMs / 60000U) + " 分钟";
  return true;
}
}  // namespace FocusTimer
