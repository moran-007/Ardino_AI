#pragma once

#include <Arduino.h>
#include <time.h>

namespace AlarmManager {

constexpr size_t MAX_ENTRIES = 12;

enum class Type : uint8_t {
  OneShot = 0,
  Recurring = 1,
  Timer = 2,
};

struct Entry {
  uint32_t id = 0;
  Type type = Type::OneShot;
  bool enabled = true;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t weekdays = 0;
  int64_t triggerAt = 0;
  int64_t lastFiredAt = 0;
  String label;
};

struct CommandResult {
  bool handled = false;
  bool changed = false;
  String message;
};

bool begin();
size_t count();
bool get(size_t index, Entry &entry);
bool addRecurring(uint8_t hour, uint8_t minute, uint8_t weekdays,
                  const String &label, uint32_t *id = nullptr);
bool addOneShot(time_t triggerAt, const String &label, uint32_t *id = nullptr);
bool addTimer(uint32_t durationSeconds, const String &label, time_t now,
              uint32_t *id = nullptr);
bool remove(uint32_t id);
bool setEnabled(uint32_t id, bool enabled);
size_t clearAll();
size_t cancelType(Type type);
bool poll(time_t now, Entry &fired);
bool ringing();
Entry ringingEntry();
void dismiss();
bool snooze(time_t now, uint32_t seconds = 300);
String toJson(time_t now);
String summary(time_t now);
String nextSummary(time_t now);
CommandResult handleNaturalLanguage(const String &input, time_t now);

}  // namespace AlarmManager
