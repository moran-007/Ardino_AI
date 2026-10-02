#include "multi_mic_config.h"
#include <Preferences.h>
#include "multi_mic_selector.h"
#include "voice_input.h"

namespace MultiMicConfig {
namespace { bool operationActive = false; }
bool busy() { return operationActive; }

bool apply(uint8_t mask, uint8_t preferred, String &message) {
  if (operationActive || !MultiMic::validConfiguration(mask, preferred)) {
    message = "[麦克风] 配置无效或检测正在进行。"; return false;
  }
  operationActive = true;
  struct Done { ~Done() { operationActive = false; } } done;
  const uint8_t oldMask = VoiceInput::microphoneMask();
  const uint8_t oldPreferred = VoiceInput::preferredMicrophone();
  if (!VoiceInput::configureMicrophones(mask, preferred)) {
    message = "[麦克风] 配置未切换：正在采集或 I2S 初始化失败。"; return false;
  }
  if ((mask & (mask - 1U)) && !VoiceInput::probeMicrophones(1500)) {
    const String diagnostics = VoiceInput::microphoneDiagnostics();
    const bool restored = VoiceInput::configureMicrophones(oldMask, oldPreferred);
    message = String(restored ? "[麦克风] 检测未通过，已恢复原配置。请对各麦克风说话并检查接线。\n" :
                               "[麦克风] 检测未通过，恢复原配置失败，请重启设备。\n") + diagnostics;
    return false;
  }
  Preferences stored;
  bool saved = false;
  if (stored.begin("multi-mic", false)) {
    const uint16_t config = static_cast<uint16_t>(mask) | (static_cast<uint16_t>(preferred) << 8);
    saved = stored.putUShort("config", config) == sizeof(config);
    stored.end();
  }
  if (!saved) {
    VoiceInput::configureMicrophones(oldMask, oldPreferred);
    message = "[麦克风] NVS 保存失败，已恢复原配置。"; return false;
  }
  message = String("[麦克风] 配置已保存，重启后保持。\n") + VoiceInput::microphoneDiagnostics();
  return true;
}


bool begin() {
  Preferences stored;
  if (!stored.begin("multi-mic", false)) return false;
  const uint16_t config = stored.getUShort("config", MultiMic::DEFAULT_INSTALLED_MASK);
  const uint8_t mask = config & 0xffU;
  const uint8_t preferred = config >> 8;
  stored.end();
  return MultiMic::validConfiguration(mask, preferred)
      ? VoiceInput::configureMicrophones(mask, preferred)
      : VoiceInput::configureMicrophones(MultiMic::DEFAULT_INSTALLED_MASK, 0);
}

bool executeCommand(const String &command, String &message) {
  if (command == "status" || command.isEmpty()) {
    message = VoiceInput::microphoneDiagnostics();
    return true;
  }
  if (command == "test") {
    const bool ok = VoiceInput::probeMicrophones(500);
    message = ok ? VoiceInput::microphoneDiagnostics()
                 : String("[麦克风] 测试失败或已取消，请检查状态和接线。");
    return ok;
  }
  uint8_t mask = VoiceInput::microphoneMask();
  uint8_t preferred = VoiceInput::preferredMicrophone();
  bool recognized = false;
  if (command == "count 1" || command == "count 2" || command == "count 3") {
    mask = command == "count 1" ? 1 : command == "count 2" ? 3 : 7;
    preferred = 0;
    recognized = true;
  } else {
    for (uint8_t value = 1; value <= 7; ++value) {
      if (command == String("mask ") + String(value)) {
        mask = value;
        preferred = 0;
        recognized = true;
      }
    }
    if (command == "select auto") { preferred = 0; recognized = true; }
    for (uint8_t value = 1; value <= 3; ++value) {
      if (command == String("select ") + String(value)) {
        preferred = value;
        recognized = true;
      }
    }
  }
  if (!recognized || !MultiMic::validConfiguration(mask, preferred)) {
    message = "[麦克风] /mic count 1|2|3；/mic mask 1..7；/mic select auto|1|2|3；/mic test。固定通道须已安装。";
    return false;
  }
  return apply(mask, preferred, message);
}

}  // namespace MultiMicConfig
