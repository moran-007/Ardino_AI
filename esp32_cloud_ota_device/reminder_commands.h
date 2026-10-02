#pragma once

#include "alarm_manager.h"
#include "memo_creation.h"

namespace ReminderCommands {
struct Result {
  bool handled = false;
  bool memoChanged = false;
  bool alarmChanged = false;
  String message;
};

inline Result execute(const String &text, time_t now) {
  Result result;
  const MemoCreation::Intent creation = MemoCreation::parse(text);
  const auto memo = MemoManager::handleNaturalLanguage(text, now);
  result.handled = memo.handled;
  result.memoChanged = memo.changed;
  result.message = memo.message;
  // Only an explicit reminder followed by an add-todo command creates both.
  // Reminder words inside an ordinary note/todo never execute an alarm command.
  if (!memo.handled || (creation.handled && creation.reminderAlso)) {
    const auto alarm = AlarmManager::handleNaturalLanguage(creation.reminderAlso ? creation.content : text, now);
    result.handled = result.handled || alarm.handled;
    result.alarmChanged = alarm.changed;
    if (alarm.handled) {
      if (!result.message.isEmpty()) result.message += "；";
      result.message += alarm.message;
    } else if (creation.reminderAlso) {
      result.message += "；提醒未设置，请确认时间明确且设备时钟已同步";
    }
  }
  return result;
}
}  // namespace ReminderCommands
