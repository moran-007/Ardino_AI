#pragma once
#include <Arduino.h>

namespace FocusTimer {
struct Config {
  uint16_t focusMinutes = 25;
  uint16_t shortBreakMinutes = 5;
  uint16_t longBreakMinutes = 15;
  uint8_t roundsBeforeLongBreak = 4;
};
struct CommandResult {
  bool handled = false;
  bool changed = false;
  String message;
};
bool begin();
bool validConfig(const Config &config);
Config getConfig();
String toJson();
String displaySummary();
bool active();
bool focusing();
CommandResult control(const String &action, const Config *config = nullptr);
CommandResult handleNaturalLanguage(const String &input);
// Tick from loop/audio service; the caller owns when it is safe to speak.
bool poll(String &announcement);
}  // namespace FocusTimer
