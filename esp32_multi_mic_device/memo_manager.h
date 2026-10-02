#pragma once

#include <Arduino.h>
#include <FS.h>
#include <time.h>

namespace MemoManager {

constexpr size_t MAX_ENTRIES = 20;
constexpr size_t MAX_TEXT_BYTES = 180;

enum class Kind : uint8_t {
  Note = 0,
  Todo = 1,
};

struct Entry {
  uint32_t id = 0;
  Kind kind = Kind::Note;
  bool completed = false;
  int64_t createdAt = 0;
  String text;
};

struct CommandResult {
  bool handled = false;
  bool changed = false;
  String message;
};

bool begin(fs::FS &storage);
bool ready();
size_t count();
size_t count(Kind kind);
size_t pendingTodoCount();
bool get(size_t index, Entry &entry);
bool add(Kind kind, const String &text, time_t now = 0, uint32_t *id = nullptr);
bool setCompleted(uint32_t id, bool completed);
bool remove(uint32_t id);
size_t clear(Kind kind);
size_t clearCompletedTodos();
String toJson();
String summary();
String displaySummary();
String displaySummary(Kind kind);
String aiContext();
CommandResult handleNaturalLanguage(const String &input, time_t now);

}  // namespace MemoManager
