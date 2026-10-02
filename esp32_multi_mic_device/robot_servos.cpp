#include "robot_servos.h"

#include <string.h>

namespace RobotServos {
namespace {

bool initialized = false;
bool attached[COUNT] = {};
bool active[COUNT] = {};
uint8_t targetAngles[COUNT] = {};

static_assert(MIN_PULSE_US >= 500 && MAX_PULSE_US <= 2500 &&
                  MIN_PULSE_US < MAX_PULSE_US,
              "Check SG90 pulse endpoints");
static_assert(DEFAULT_ANGLES[0] <= 180 && DEFAULT_ANGLES[1] <= 180 &&
                  DEFAULT_ANGLES[2] <= 180 && DEFAULT_ANGLES[3] <= 180,
              "Servo defaults must be in 0..180 degrees");

void detachOutputs() {
  for (size_t i = 0; i < COUNT; ++i) {
    if (!attached[i]) continue;
    ledcDetach(PINS[i]);
    attached[i] = active[i] = false;
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
  }
  initialized = false;
}

bool unsignedNumber(const char *text, uint16_t maximum, uint16_t &value) {
  if (!text || !*text) return false;
  value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return false;
    const uint16_t next = value * 10U + static_cast<uint16_t>(*text - '0');
    if (next > maximum) return false;
    value = next;
  }
  return true;
}

}  // namespace

bool begin() {
  if (initialized) return true;
#if !CONFIG_IDF_TARGET_ESP32S3
  return false;  // No guessed servo pin assignments for legacy ESP32 boards.
#else
  for (size_t i = 0; i < COUNT; ++i) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
    // Auto allocation reserves four independent duty channels at 50 Hz.
    attached[i] = ledcAttach(PINS[i], PWM_HZ, PWM_BITS);
    if (!attached[i] || !ledcWrite(PINS[i], 0)) {
      detachOutputs();
      return false;
    }
  }
  initialized = true;
  return home();
#endif
}

bool ready() { return initialized; }

uint8_t angle(uint8_t channel) {
  return channel >= 1 && channel <= COUNT ? targetAngles[channel - 1] : 0;
}

bool outputActive(uint8_t channel) {
  return initialized && channel >= 1 && channel <= COUNT && active[channel - 1];
}

bool setAngle(uint8_t channel, uint16_t degrees) {
  if (!initialized || channel < 1 || channel > COUNT || degrees > 180) return false;
  const size_t i = channel - 1;
  const uint32_t pulseUs = MIN_PULSE_US +
      (static_cast<uint32_t>(MAX_PULSE_US - MIN_PULSE_US) * degrees + 90U) / 180U;
  const uint32_t periodUs = 1000000UL / PWM_HZ;
  const uint32_t duty = (pulseUs * (1UL << PWM_BITS) + periodUs / 2) / periodUs;
  if (!ledcWrite(PINS[i], duty)) {
    detachOutputs();
    return false;
  }
  targetAngles[i] = static_cast<uint8_t>(degrees);
  active[i] = true;
  return true;
}

bool setAngles(const uint16_t (&degrees)[COUNT]) {
  // Validate the whole command before emitting any pulse changes.
  if (!initialized) return false;
  for (size_t i = 0; i < COUNT; ++i) if (degrees[i] > 180) return false;
  for (size_t i = 0; i < COUNT; ++i) {
    if (!setAngle(i + 1, degrees[i])) return false;
  }
  return true;
}

bool home() {
  uint16_t degrees[COUNT];
  for (size_t i = 0; i < COUNT; ++i) degrees[i] = DEFAULT_ANGLES[i];
  return setAngles(degrees);
}

void release() {
  for (size_t i = 0; i < COUNT; ++i) {
    if (!attached[i]) continue;
    if (!ledcWrite(PINS[i], 0)) {
      detachOutputs();
      return;
    }
    active[i] = false;
  }
}

String status() {
  String result = initialized ? "[舵机] SG90 已就绪" : "[舵机] SG90 初始化/输出失败";
  for (size_t i = 0; i < COUNT; ++i) {
    result += "\n  " + String(i + 1) + "号 GPIO" + String(PINS[i]) + ": ";
    result += active[i] ? String(targetAngles[i]) + "度（指令角度）" : "PWM关闭";
  }
  return result;
}

bool executeCommand(const String &command, String &message) {
  if (command.isEmpty() || command == "status") {
    message = status();
    return true;
  }
  if (command == "off") {
    release();
    message = "[舵机] 四路 PWM 已关闭；舵机电源仍接通。";
    return true;
  }
  bool ok = false;
  if (command == "home" || command == "on") {
    ok = home();
  } else {
    if (command.length() >= 64) {
      message = "[舵机] 命令过长。";
      return false;
    }
    char buffer[64];
    command.toCharArray(buffer, sizeof(buffer));
    char *tokens[5];
    size_t count = 0;
    char *context = nullptr;
    char *token = strtok_r(buffer, " \t", &context);
    while (token && count < 5) {
      tokens[count++] = token;
      token = strtok_r(nullptr, " \t", &context);
    }
    if (!token && count == 5 && strcmp(tokens[0], "all") == 0) {
      uint16_t degrees[COUNT];
      bool valid = true;
      for (size_t i = 0; i < COUNT; ++i)
        valid = unsignedNumber(tokens[i + 1], 180, degrees[i]) && valid;
      if (valid) ok = setAngles(degrees);
    } else if (!token && count == 2) {
      uint16_t channel, degrees;
      if (unsignedNumber(tokens[0], COUNT, channel) && channel >= 1 &&
          unsignedNumber(tokens[1], 180, degrees)) ok = setAngle(channel, degrees);
    }
  }
  message = ok ? status() :
      "[舵机] 未执行：请检查模块状态；命令为 /servo 编号(1~4) 角度(0~180)、/servo all 角1 角2 角3 角4、/servo home、/servo off。";
  return ok;
}

}  // namespace RobotServos
