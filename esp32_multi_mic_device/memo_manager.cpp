#include "memo_manager.h"
#include "memo_creation.h"

#include <ArduinoJson.h>

namespace MemoManager {
namespace {

constexpr char STORE_PATH[] = "/memos.json";
constexpr char TEMP_PATH[] = "/memos.tmp";
constexpr char BACKUP_PATH[] = "/memos.bak";

fs::FS *memoStorage = nullptr;
Entry entries[MAX_ENTRIES];
size_t entryCount = 0;
uint32_t nextId = 1;
bool initialized = false;

String clipUtf8(String value, size_t maximumBytes) {
  value.trim();
  value.replace("\r", " ");
  value.replace("\n", " ");
  while (value.indexOf("  ") >= 0) value.replace("  ", " ");
  if (value.length() <= maximumBytes) return value;
  size_t end = maximumBytes;
  while (end > 0 && end < value.length() &&
         (static_cast<uint8_t>(value[end]) & 0xC0) == 0x80) {
    --end;
  }
  value.remove(end);
  value.trim();
  return value;
}

bool save() {
  if (!memoStorage) return false;
  JsonDocument doc;
  doc["version"] = 1;
  doc["next_id"] = nextId;
  JsonArray list = doc["items"].to<JsonArray>();
  for (size_t i = 0; i < entryCount; ++i) {
    JsonObject item = list.add<JsonObject>();
    item["id"] = entries[i].id;
    item["kind"] = entries[i].kind == Kind::Todo ? "todo" : "note";
    item["completed"] = entries[i].completed;
    item["created_at"] = entries[i].createdAt;
    item["text"] = entries[i].text;
  }

  memoStorage->remove(TEMP_PATH);
  File file = memoStorage->open(TEMP_PATH, FILE_WRITE);
  if (!file) return false;
  const size_t written = serializeJson(doc, file);
  file.flush();
  file.close();
  if (written == 0) {
    memoStorage->remove(TEMP_PATH);
    return false;
  }

  memoStorage->remove(BACKUP_PATH);
  const bool hadCurrent = memoStorage->exists(STORE_PATH);
  if (hadCurrent && !memoStorage->rename(STORE_PATH, BACKUP_PATH)) {
    memoStorage->remove(TEMP_PATH);
    return false;
  }
  if (!memoStorage->rename(TEMP_PATH, STORE_PATH)) {
    if (hadCurrent) memoStorage->rename(BACKUP_PATH, STORE_PATH);
    return false;
  }
  memoStorage->remove(BACKUP_PATH);
  return true;
}

bool loadFrom(const char *path) {
  File file = memoStorage->open(path, FILE_READ);
  if (!file) return false;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, file);
  file.close();
  if (error || !doc["items"].is<JsonArrayConst>()) return false;

  Entry loaded[MAX_ENTRIES];
  size_t loadedCount = 0;
  uint32_t maximumId = 0;
  for (JsonObjectConst item : doc["items"].as<JsonArrayConst>()) {
    if (loadedCount >= MAX_ENTRIES) break;
    const uint32_t id = item["id"] | 0U;
    String text = item["text"] | "";
    text = clipUtf8(text, MAX_TEXT_BYTES);
    if (id == 0 || text.isEmpty()) continue;
    loaded[loadedCount].id = id;
    loaded[loadedCount].kind = String(item["kind"] | "note") == "todo"
                                   ? Kind::Todo
                                   : Kind::Note;
    loaded[loadedCount].completed = loaded[loadedCount].kind == Kind::Todo &&
                                    static_cast<bool>(item["completed"] | false);
    loaded[loadedCount].createdAt = item["created_at"] | 0LL;
    loaded[loadedCount].text = text;
    maximumId = max(maximumId, id);
    ++loadedCount;
  }
  entryCount = loadedCount;
  for (size_t i = 0; i < entryCount; ++i) entries[i] = loaded[i];
  nextId = max(static_cast<uint32_t>(doc["next_id"] | 1U), maximumId + 1U);
  if (nextId == 0) nextId = 1;
  return true;
}

int findById(uint32_t id) {
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int findOrdinal(Kind kind, uint32_t ordinal) {
  if (ordinal == 0) return -1;
  uint32_t current = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].kind != kind) continue;
    if (++current == ordinal) return static_cast<int>(i);
  }
  return -1;
}

uint32_t chineseNumber(const String &value) {
  static const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  if (value.indexOf("十") >= 0) {
    const int tenAt = value.indexOf("十");
    uint32_t tens = 1;
    uint32_t ones = 0;
    for (uint32_t i = 1; i <= 9; ++i) {
      const int at = value.indexOf(digits[i]);
      if (at >= 0 && at < tenAt) tens = i;
      if (at > tenAt) ones = i;
    }
    return tens * 10 + ones;
  }
  for (uint32_t i = 1; i <= 9; ++i) {
    if (value.indexOf(digits[i]) >= 0) return i;
  }
  return 0;
}

uint32_t commandNumber(const String &value) {
  uint32_t number = 0;
  bool found = false;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] >= '0' && value[i] <= '9') {
      found = true;
      number = number * 10U + static_cast<uint32_t>(value[i] - '0');
      if (number > 100000U) break;
    } else if (found) {
      break;
    }
  }
  return found ? number : chineseNumber(value);
}

String itemList(Kind kind) {
  String output;
  uint32_t ordinal = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].kind != kind) continue;
    if (!output.isEmpty()) output += "；";
    output += String(++ordinal) + "、";
    if (kind == Kind::Todo) output += entries[i].completed ? "已完成 " : "未完成 ";
    output += entries[i].text;
  }
  if (output.isEmpty()) output = kind == Kind::Todo ? "当前没有待办事项" : "当前没有便签";
  return output;
}

String boundedItemList(Kind kind, size_t maximumBytes) {
  String output;
  uint32_t ordinal = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].kind != kind) continue;
    String item = String(++ordinal) + "、";
    if (kind == Kind::Todo) item += entries[i].completed ? "已完成 " : "未完成 ";
    item += entries[i].text;
    const size_t separatorBytes = output.isEmpty() ? 0 : String("；").length();
    if (output.length() + separatorBytes + item.length() > maximumBytes) {
      if (output.isEmpty()) output = clipUtf8(item, maximumBytes);
      else if (output.length() + String("；…").length() <= maximumBytes) output += "；…";
      break;
    }
    if (!output.isEmpty()) output += "；";
    output += item;
  }
  return output.isEmpty() ? "无" : output;
}

CommandResult result(bool changed, const String &message) {
  CommandResult output;
  output.handled = true;
  output.changed = changed;
  output.message = message;
  return output;
}

}  // namespace

bool begin(fs::FS &storage) {
  memoStorage = &storage;
  entryCount = 0;
  nextId = 1;
  initialized = true;
  if (memoStorage->exists(STORE_PATH) && loadFrom(STORE_PATH)) return true;
  if (memoStorage->exists(BACKUP_PATH) && loadFrom(BACKUP_PATH)) {
    memoStorage->remove(STORE_PATH);
    initialized = memoStorage->rename(BACKUP_PATH, STORE_PATH);
    if (initialized) memoStorage->remove(TEMP_PATH);
    Serial.println(initialized ? F("[便签] 已从备份恢复。")
                               : F("[便签] 备份有效，但恢复写入失败。"));
    return initialized;
  }
  if (memoStorage->exists(TEMP_PATH) && loadFrom(TEMP_PATH)) {
    memoStorage->remove(STORE_PATH);
    initialized = memoStorage->rename(TEMP_PATH, STORE_PATH);
    if (initialized) memoStorage->remove(BACKUP_PATH);
    Serial.println(initialized ? F("[便签] 已从未完成写入文件恢复。")
                               : F("[便签] 临时文件有效，但恢复写入失败。"));
    return initialized;
  }
  if (!memoStorage->exists(STORE_PATH)) {
    memoStorage->remove(TEMP_PATH);
    initialized = save();
    return initialized;
  }
  Serial.println(F("[便签] 存储文件无法解析；保留原文件并使用空列表。"));
  initialized = false;
  return false;
}

bool ready() {
  return initialized;
}

size_t count() {
  return entryCount;
}

size_t count(Kind kind) {
  size_t total = 0;
  for (size_t i = 0; i < entryCount; ++i) total += entries[i].kind == kind;
  return total;
}

size_t pendingTodoCount() {
  size_t total = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    total += entries[i].kind == Kind::Todo && !entries[i].completed;
  }
  return total;
}

bool get(size_t index, Entry &entry) {
  if (index >= entryCount) return false;
  entry = entries[index];
  return true;
}

bool add(Kind kind, const String &text, time_t now, uint32_t *id) {
  if (!initialized || entryCount >= MAX_ENTRIES) return false;
  const String clean = clipUtf8(text, MAX_TEXT_BYTES);
  if (clean.isEmpty()) return false;
  const uint32_t previousNextId = nextId;
  Entry entry;
  entry.id = nextId++;
  if (entry.id == 0) entry.id = nextId++;
  entry.kind = kind;
  entry.completed = false;
  entry.createdAt = static_cast<int64_t>(now);
  entry.text = clean;
  entries[entryCount++] = entry;
  if (!save()) {
    --entryCount;
    nextId = previousNextId;
    return false;
  }
  if (id) *id = entry.id;
  return true;
}

bool setCompleted(uint32_t id, bool completed) {
  const int index = findById(id);
  if (!initialized || index < 0 || entries[index].kind != Kind::Todo) return false;
  const bool previous = entries[index].completed;
  entries[index].completed = completed;
  if (!save()) {
    entries[index].completed = previous;
    return false;
  }
  return true;
}

bool remove(uint32_t id) {
  const int index = findById(id);
  if (!initialized || index < 0) return false;
  const Entry removed = entries[index];
  for (size_t i = static_cast<size_t>(index); i + 1 < entryCount; ++i) entries[i] = entries[i + 1];
  --entryCount;
  if (!save()) {
    for (size_t i = entryCount; i > static_cast<size_t>(index); --i) entries[i] = entries[i - 1];
    entries[index] = removed;
    ++entryCount;
    return false;
  }
  return true;
}

size_t clear(Kind kind) {
  if (!initialized) return 0;
  Entry kept[MAX_ENTRIES];
  size_t keptCount = 0;
  size_t removed = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].kind == kind) ++removed;
    else kept[keptCount++] = entries[i];
  }
  if (removed == 0) return 0;
  Entry previous[MAX_ENTRIES];
  const size_t previousCount = entryCount;
  for (size_t i = 0; i < previousCount; ++i) previous[i] = entries[i];
  for (size_t i = 0; i < keptCount; ++i) entries[i] = kept[i];
  entryCount = keptCount;
  if (!save()) {
    for (size_t i = 0; i < previousCount; ++i) entries[i] = previous[i];
    entryCount = previousCount;
    return 0;
  }
  return removed;
}

size_t clearCompletedTodos() {
  if (!initialized) return 0;
  Entry kept[MAX_ENTRIES];
  size_t keptCount = 0;
  size_t removed = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].kind == Kind::Todo && entries[i].completed) ++removed;
    else kept[keptCount++] = entries[i];
  }
  if (removed == 0) return 0;
  Entry previous[MAX_ENTRIES];
  const size_t previousCount = entryCount;
  for (size_t i = 0; i < previousCount; ++i) previous[i] = entries[i];
  for (size_t i = 0; i < keptCount; ++i) entries[i] = kept[i];
  entryCount = keptCount;
  if (!save()) {
    for (size_t i = 0; i < previousCount; ++i) entries[i] = previous[i];
    entryCount = previousCount;
    return 0;
  }
  return removed;
}

String toJson() {
  JsonDocument doc;
  doc["ok"] = initialized;
  doc["count"] = entryCount;
  doc["notes"] = count(Kind::Note);
  doc["todos"] = count(Kind::Todo);
  doc["pending_todos"] = pendingTodoCount();
  JsonArray list = doc["items"].to<JsonArray>();
  for (size_t i = 0; i < entryCount; ++i) {
    JsonObject item = list.add<JsonObject>();
    item["id"] = entries[i].id;
    item["kind"] = entries[i].kind == Kind::Todo ? "todo" : "note";
    item["completed"] = entries[i].completed;
    item["created_at"] = entries[i].createdAt;
    item["text"] = entries[i].text;
  }
  String output;
  serializeJson(doc, output);
  return output;
}

String summary() {
  return "便签 " + String(count(Kind::Note)) + " 条，待办 " +
         String(count(Kind::Todo)) + " 条（未完成 " + String(pendingTodoCount()) + " 条）";
}

String displaySummary() {
  String output;
  size_t shown = 0;
  for (size_t cursor = entryCount; cursor > 0 && shown < 4; --cursor) {
    const size_t i = cursor - 1;
    if (!output.isEmpty()) output += '\n';
    output += entries[i].kind == Kind::Note ? "便: " : entries[i].completed ? "待(完): " : "待: ";
    output += clipUtf8(entries[i].text, 42);
    ++shown;
  }
  return output.isEmpty() ? "暂无便签和待办" : output;
}

String displaySummary(Kind kind) {
  String output;
  size_t shown = 0, ordinal = count(kind);
  for (size_t cursor = entryCount; cursor > 0 && shown < 4; --cursor) {
    const Entry &entry = entries[cursor - 1];
    if (entry.kind != kind) continue;
    if (!output.isEmpty()) output += '\n';
    output += String(ordinal--) + ". ";
    if (kind == Kind::Todo && entry.completed) output += "已完成 ";
    output += entry.text;
    ++shown;
  }
  return output;
}

String aiContext() {
  String output = summary() + "；便签：" + boundedItemList(Kind::Note, 170) +
                  "；待办：" + boundedItemList(Kind::Todo, 170);
  output.reserve(512);
  return clipUtf8(output, 480);
}

CommandResult handleNaturalLanguage(const String &input, time_t now) {
  String text = input;
  text.trim();
  if (text.isEmpty()) return CommandResult();
  text.replace("代办", "待办");
  const MemoCreation::Intent creation = MemoCreation::parse(text);
  if (creation.handled) {
    const String content = clipUtf8(creation.content, MAX_TEXT_BYTES);
    const bool changed = add(creation.kind, content, now);
    return result(changed, changed ? (creation.kind == Kind::Todo ? "待办已添加：" : "便签已保存：") + content
                                 : "保存失败：内容为空、列表已满或存储写入失败");
  }
  const bool mentionsTodo = text.indexOf("待办") >= 0;
  const bool mentionsNote = text.indexOf("便签") >= 0 || text.indexOf("记事") >= 0;

  if (mentionsTodo && (text.indexOf("清理已完成") >= 0 ||
                       text.indexOf("清空已完成") >= 0 ||
                       text.indexOf("清除已完成") >= 0 ||
                       text.indexOf("删除已完成") >= 0)) {
    const size_t removed = clearCompletedTodos();
    return result(removed > 0, removed > 0 ? "已删除" + String(removed) + "条已完成待办"
                                          : "没有已完成的待办");
  }
  if ((mentionsTodo || mentionsNote) && text.indexOf("清空") >= 0) {
    const Kind kind = mentionsTodo ? Kind::Todo : Kind::Note;
    const size_t removed = clear(kind);
    return result(removed > 0, removed > 0 ? "已清空" + String(removed) +
                                                (kind == Kind::Todo ? "条待办" : "条便签")
                                          : (kind == Kind::Todo ? "当前没有待办" : "当前没有便签"));
  }
  if ((mentionsTodo || mentionsNote) &&
      (text.indexOf("查看") >= 0 || text.indexOf("查询") >= 0 ||
       text.indexOf("有哪些") >= 0 || text.indexOf("读一下") >= 0)) {
    const Kind kind = mentionsTodo ? Kind::Todo : Kind::Note;
    return result(false, itemList(kind));
  }

  if (mentionsTodo && (text.indexOf("完成") >= 0 || text.indexOf("做完") >= 0 ||
                       text.indexOf("恢复") >= 0 || text.indexOf("未完成") >= 0)) {
    const uint32_t ordinal = commandNumber(text);
    const int index = findOrdinal(Kind::Todo, ordinal);
    if (index < 0) return result(false, "没有找到对应的待办");
    const bool completed = text.indexOf("恢复") < 0 && text.indexOf("未完成") < 0;
    const bool changed = setCompleted(entries[index].id, completed);
    return result(changed, changed ? String("已将第") + ordinal + "条待办标记为" +
                                         (completed ? "完成" : "未完成")
                                   : "待办状态保存失败");
  }

  if ((mentionsTodo || mentionsNote) &&
      (text.indexOf("删除") >= 0 || text.indexOf("取消") >= 0)) {
    const Kind kind = mentionsTodo ? Kind::Todo : Kind::Note;
    const uint32_t ordinal = commandNumber(text);
    const int index = findOrdinal(kind, ordinal);
    if (index < 0) return result(false, kind == Kind::Todo ? "没有找到对应的待办" : "没有找到对应的便签");
    const bool changed = remove(entries[index].id);
    return result(changed, changed ? String("已删除第") + ordinal +
                                         (kind == Kind::Todo ? "条待办" : "条便签")
                                   : "删除失败");
  }

  return CommandResult();
}

}  // namespace MemoManager
