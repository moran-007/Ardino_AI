#include "robot_interaction.h"
#include "robot_servos.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace RobotInteraction {
namespace {
constexpr char STORE_NAMESPACE[] = "interaction";
constexpr char STORE_KEY[] = "config";
constexpr uint32_t STEP_MS = 40, MAX_DURATION_MS = 4000;
Config settings;
bool initialized = false, moving = false;
uint8_t originalAngle = 0, commandedAngle = 0, stage = 0;
uint32_t lastStep = 0, started = 0;
String errorText, messageText;

bool valid(const Config &value) {
  return value.channel >= 1 && value.channel <= RobotServos::COUNT &&
      value.lowDegrees <= 180 && value.highDegrees <= 180 &&
      value.highDegrees > value.lowDegrees &&
      value.highDegrees - value.lowDegrees <= 30;
}

bool stillOwned() {
  return RobotServos::ready() && RobotServos::outputActive(settings.channel) &&
      RobotServos::angle(settings.channel) == commandedAngle;
}

bool fail(const char *message) { errorText = message; messageText = message; return false; }

void lostControl() {
  moving = false;
  errorText = "";
  messageText = "已停止互动动作，保留手动角度或关闭的 PWM";
}

uint8_t target() {
  return stage == 0 ? settings.highDegrees : stage == 1 ? settings.lowDegrees : originalAngle;
}

void finished() { moving = false;messageText = "互动动作已完成，已回到原指令角度"; }
}  // namespace

bool begin() {
  settings = Config();moving = false;initialized = false;errorText = "";
  messageText = "互动动作默认关闭，请先校准单个舵机的安全范围";
  Preferences storage;
  if (!storage.begin(STORE_NAMESPACE, false)) return fail("互动动作设置存储未就绪");
  const String payload = storage.getString(STORE_KEY, "");
  storage.end();
  initialized = true;
  if (payload.isEmpty()) return true;
  JsonDocument doc;
  if (payload.length() > 256 || deserializeJson(doc, payload.c_str()) ||
      !doc["version"].is<int>() || doc["version"].as<int>() != 1 ||
      !doc["enabled"].is<bool>() || !doc["channel"].is<int>() ||
      !doc["low_degrees"].is<int>() || !doc["high_degrees"].is<int>())
    return fail("互动动作设置损坏，已恢复默认关闭；请重新保存");
  const int channel = doc["channel"].as<int>(), low = doc["low_degrees"].as<int>(), high = doc["high_degrees"].as<int>();
  if (channel < 1 || channel > 4 || low < 0 || low > 180 || high < 0 || high > 180)
    return fail("互动动作范围无效，已恢复默认关闭");
  Config loaded;loaded.enabled = doc["enabled"].as<bool>();
  loaded.channel = channel;loaded.lowDegrees = low;loaded.highDegrees = high;
  if (!valid(loaded)) return fail("互动动作范围无效，已恢复默认关闭");
  settings = loaded;
  messageText = settings.enabled ? "互动动作已启用，等待下一次问候、鼓励或庆祝" : "互动动作已关闭";
  return true;
}

bool configure(const Config &value) {
  if (!initialized) return fail("互动动作设置未初始化");
  if (!valid(value)) return fail("请选择 1 至 4 号舵机；范围需在 0 至 180 度内，上限大于下限，跨度不超过 30 度");
  if (moving) return fail("互动动作进行中，请稍后保存设置");
  JsonDocument doc;
  doc["version"] = 1;doc["enabled"] = value.enabled;doc["channel"] = value.channel;
  doc["low_degrees"] = value.lowDegrees;doc["high_degrees"] = value.highDegrees;
  char json[256];serializeJson(doc, json, sizeof(json));const String payload(json);
  Preferences storage;
  if (!storage.begin(STORE_NAMESPACE, false)) return fail("无法打开互动动作设置存储");
  const bool written = storage.putString(STORE_KEY, payload) == payload.length();
  storage.end();
  if (!written) return fail("互动动作设置保存失败，原设置保留");
  settings = value;errorText = "";
  messageText = settings.enabled ? "设置已保存；仅在校准范围内执行小幅动作" : "设置已保存，互动动作已关闭";
  return true;
}

bool onReply(uint8_t mood) {
  if (!initialized || !settings.enabled) return false;
  if (mood != 0 && mood != 4 && mood != 45) return false;
  if (moving) return fail("互动动作正在进行");
  if (!RobotServos::ready() || !RobotServos::outputActive(settings.channel))
    return fail("选定舵机的 PWM 未开启，已跳过互动动作");
  const uint8_t current = RobotServos::angle(settings.channel);
  if (current < settings.lowDegrees || current > settings.highDegrees)
    return fail("当前舵机指令角度不在校准范围内，已跳过互动动作");
  originalAngle = commandedAngle = current;stage = 0;
  started = lastStep = millis();moving = true;errorText = "";
  messageText = "正在执行单舵机小幅互动动作";
  return true;
}

void poll() {
  if (!moving) return;
  if (!stillOwned()) { lostControl();return; }
  const uint32_t now = millis();
  if (now - started >= MAX_DURATION_MS) {
    stop(true);
    if (errorText.isEmpty()) messageText = "互动动作等待超时，已回到原指令角度";
    return;
  }
  while (commandedAngle == target()) {
    if (stage == 2) { finished();return; }
    ++stage;
  }
  if (now - lastStep < STEP_MS) return;
  // Never catch up missed ticks with a fast sweep: one degree per call/tick.
  const uint8_t next = commandedAngle < target() ? commandedAngle + 1 : commandedAngle - 1;
  if (!RobotServos::setAngle(settings.channel, next)) {
    moving = false;fail("互动动作输出失败，舵机 PWM 已关闭");return;
  }
  commandedAngle = next;lastStep = now;
  if (stage == 2 && commandedAngle == originalAngle) finished();
}

void stop(bool restore) {
  if (!moving) return;
  if (!stillOwned()) { lostControl();return; }
  moving = false;
  if (restore && commandedAngle != originalAngle) {
    if (!RobotServos::setAngle(settings.channel, originalAngle)) {
      fail("互动动作停止后恢复角度失败，舵机 PWM 已关闭");return;
    }
    commandedAngle = originalAngle;
  }
  errorText = "";messageText = restore ? "互动动作已停止，已回到原指令角度" : "互动动作已停止，保留当前指令角度";
}

bool busy() { return moving; }
const char *lastError() { return errorText.c_str(); }
String toJson() {
  JsonDocument doc;
  doc["initialized"] = initialized;doc["enabled"] = settings.enabled;
  doc["channel"] = settings.channel;doc["low_degrees"] = settings.lowDegrees;
  doc["high_degrees"] = settings.highDegrees;doc["busy"] = moving;
  doc["servo_ready"] = RobotServos::ready();
  doc["servo_output_active"] = RobotServos::outputActive(settings.channel);
  doc["current_degrees"] = RobotServos::angle(settings.channel);
  doc["message"] = messageText.c_str();
  char json[768];serializeJson(doc, json, sizeof(json));return String(json);
}
}  // namespace RobotInteraction
