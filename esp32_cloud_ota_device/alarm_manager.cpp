#include "alarm_manager.h"

#include <ArduinoJson.h>
#include <Preferences.h>

namespace AlarmManager {
namespace {

constexpr char NVS_NAMESPACE[] = "robot-clock";
constexpr char NVS_ENTRIES_KEY[] = "entries";
constexpr char NVS_NEXT_ID_KEY[] = "next_id";
constexpr int64_t CLOCK_READY_EPOCH = 1700000000LL;
constexpr int64_t MISSED_ALARM_GRACE_SECONDS = 24LL * 60LL * 60LL;

Entry entries[MAX_ENTRIES];
size_t entryCount = 0;
uint32_t nextId = 1;
bool activeRing = false;
Entry activeEntry;

String clipUtf8(String value, size_t maximumBytes) {
  if (value.length() <= maximumBytes) return value;
  size_t end = maximumBytes;
  while (end > 0 && (static_cast<uint8_t>(value[end]) & 0xC0) == 0x80) --end;
  value.remove(end);
  return value;
}

bool persist() {
  JsonDocument doc;
  JsonArray list = doc["alarms"].to<JsonArray>();
  for (size_t i = 0; i < entryCount; ++i) {
    JsonObject item = list.add<JsonObject>();
    item["id"] = entries[i].id;
    item["type"] = static_cast<uint8_t>(entries[i].type);
    item["enabled"] = entries[i].enabled;
    item["hour"] = entries[i].hour;
    item["minute"] = entries[i].minute;
    item["weekdays"] = entries[i].weekdays;
    item["trigger_at"] = entries[i].triggerAt;
    item["last_fired_at"] = entries[i].lastFiredAt;
    item["label"] = entries[i].label;
  }
  String payload;
  serializeJson(doc, payload);
  Preferences storage;
  if (!storage.begin(NVS_NAMESPACE, false)) return false;
  const bool ok = storage.putString(NVS_ENTRIES_KEY, payload) > 0 &&
                  storage.putUInt(NVS_NEXT_ID_KEY, nextId) > 0;
  storage.end();
  return ok;
}

String twoDigits(int value) {
  char output[3];
  snprintf(output, sizeof(output), "%02d", value);
  return String(output);
}

String formatClock(uint8_t hour, uint8_t minute) {
  return twoDigits(hour) + ":" + twoDigits(minute);
}

String formatLocalTime(time_t value) {
  struct tm local {};
  if (value < CLOCK_READY_EPOCH || !localtime_r(&value, &local)) return "时间待同步";
  char output[32];
  strftime(output, sizeof(output), "%m-%d %H:%M", &local);
  return String(output);
}

String weekdayDescription(uint8_t mask) {
  if (mask == 0x7F) return "每天";
  if (mask == 0x3E) return "工作日";
  if (mask == 0x41) return "周末";
  static const char *names[] = {"日", "一", "二", "三", "四", "五", "六"};
  String value = "周";
  bool any = false;
  for (uint8_t day = 0; day < 7; ++day) {
    if (mask & (1U << day)) {
      value += names[day];
      any = true;
    }
  }
  return any ? value : "未选择";
}

time_t recurringNext(const Entry &entry, time_t now) {
  if (!entry.enabled || entry.type != Type::Recurring || !entry.weekdays ||
      now < CLOCK_READY_EPOCH) return 0;
  struct tm base {};
  if (!localtime_r(&now, &base)) return 0;
  for (int add = 0; add <= 7; ++add) {
    struct tm candidate = base;
    candidate.tm_mday += add;
    candidate.tm_hour = entry.hour;
    candidate.tm_min = entry.minute;
    candidate.tm_sec = 0;
    candidate.tm_isdst = -1;
    const time_t value = mktime(&candidate);
    struct tm normalized {};
    localtime_r(&value, &normalized);
    if ((entry.weekdays & (1U << normalized.tm_wday)) && value > now) return value;
  }
  return 0;
}

time_t nextAt(const Entry &entry, time_t now) {
  if (!entry.enabled) return 0;
  if (entry.type == Type::Recurring) return recurringNext(entry, now);
  return entry.triggerAt > now ? static_cast<time_t>(entry.triggerAt) : 0;
}

int indexForId(uint32_t id) {
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

bool appendEntry(Entry entry, uint32_t *id) {
  if (entryCount >= MAX_ENTRIES) return false;
  entry.id = nextId++;
  if (nextId == 0) nextId = 1;
  entry.label = clipUtf8(entry.label.isEmpty() ? String("提醒") : entry.label, 72);
  entries[entryCount++] = entry;
  if (!persist()) {
    --entryCount;
    --nextId;
    return false;
  }
  if (id) *id = entry.id;
  return true;
}

String compactText(String value) {
  value.trim();
  value.replace(" ", "");
  value.replace("\t", "");
  value.replace("：", ":");
  return value;
}

String chineseNumber(int value) {
  static const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  if (value < 10) return digits[value];
  if (value < 20) return String("十") + (value == 10 ? "" : digits[value - 10]);
  if (value < 100) {
    const int tens = value / 10;
    const int ones = value % 10;
    return String(digits[tens]) + "十" + (ones ? digits[ones] : "");
  }
  return String(value);
}

int findNumberWithUnit(const String &text, const String &unit, int maximum) {
  int searchFrom = 0;
  while (searchFrom < static_cast<int>(text.length())) {
    const int unitAt = text.indexOf(unit, searchFrom);
    if (unitAt < 0) break;
    int start = unitAt;
    while (start > 0 && isDigit(static_cast<unsigned char>(text[start - 1]))) --start;
    if (start < unitAt) {
      const int value = text.substring(start, unitAt).toInt();
      if (value >= 0 && value <= maximum) return value;
    }
    searchFrom = unitAt + unit.length();
  }
  // Chinese number spelling handled below is intentionally limited to two
  // digits. Larger values are accepted in Arabic numerals by the scan above.
  const int chineseMaximum = maximum < 99 ? maximum : 99;
  for (int value = chineseMaximum; value >= 0; --value) {
    if (text.indexOf(String(value) + unit) >= 0 ||
        text.indexOf(chineseNumber(value) + unit) >= 0) return value;
    if (value == 2 && text.indexOf(String("两") + unit) >= 0) return value;
  }
  return -1;
}

bool parseClockTime(const String &text, int &hour, int &minute, int *matchPosition = nullptr) {
  hour = -1;
  minute = 0;
  int marker = text.indexOf(':');
  if (marker > 0) {
    int start = marker - 1;
    while (start > 0 && isDigit(static_cast<unsigned char>(text[start - 1]))) --start;
    String hourText = text.substring(start, marker);
    int end = marker + 1;
    while (end < static_cast<int>(text.length()) && isDigit(static_cast<unsigned char>(text[end]))) ++end;
    String minuteText = text.substring(marker + 1, end);
    if (!hourText.isEmpty() && !minuteText.isEmpty()) {
      hour = hourText.toInt();
      minute = minuteText.toInt();
      if (matchPosition) *matchPosition = start;
    }
  }
  if (hour < 0) {
    for (int value = 23; value >= 0 && hour < 0; --value) {
      String tokens[3] = {String(value), chineseNumber(value), value == 2 ? String("两") : String()};
      for (const String &token : tokens) {
        if (token.isEmpty()) continue;
        const String key = token + "点";
        const int position = text.indexOf(key);
        if (position < 0) continue;
        hour = value;
        if (matchPosition) *matchPosition = position;
        const String rest = text.substring(position + key.length());
        if (rest.startsWith("半")) minute = 30;
        else if (rest.startsWith("一刻")) minute = 15;
        else if (rest.startsWith("三刻")) minute = 45;
        else {
          int digitsEnd = 0;
          while (digitsEnd < static_cast<int>(rest.length()) &&
                 isDigit(static_cast<unsigned char>(rest[digitsEnd]))) ++digitsEnd;
          if (digitsEnd > 0) minute = rest.substring(0, digitsEnd).toInt();
          else {
            for (int candidate = 59; candidate >= 1; --candidate) {
              if (rest.startsWith(chineseNumber(candidate))) {
                minute = candidate;
                break;
              }
            }
          }
        }
        break;
      }
    }
  }
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
  if ((text.indexOf("下午") >= 0 || text.indexOf("晚上") >= 0 ||
       text.indexOf("今晚") >= 0 || text.indexOf("傍晚") >= 0) && hour < 12) hour += 12;
  if (text.indexOf("中午") >= 0 && hour < 11) hour += 12;
  if (text.indexOf("凌晨12") >= 0 || text.indexOf("凌晨十二") >= 0) hour = 0;
  return true;
}

uint8_t weekdayMaskFromText(const String &text) {
  if (text.indexOf("每天") >= 0 || text.indexOf("每日") >= 0) return 0x7F;
  if (text.indexOf("工作日") >= 0 || text.indexOf("周一到周五") >= 0 ||
      text.indexOf("星期一到星期五") >= 0) return 0x3E;
  if (text.indexOf("周末") >= 0) return 0x41;
  static const char *shortNames[] = {"日", "一", "二", "三", "四", "五", "六"};
  static const char *longNames[] = {"天", "一", "二", "三", "四", "五", "六"};
  uint8_t mask = 0;
  for (uint8_t day = 0; day < 7; ++day) {
    if (text.indexOf(String("周") + shortNames[day]) >= 0 ||
        text.indexOf(String("星期") + longNames[day]) >= 0) mask |= 1U << day;
  }
  return mask;
}

time_t localTimeFor(time_t now, int hour, int minute, int dayOffset) {
  struct tm value {};
  if (!localtime_r(&now, &value)) return 0;
  value.tm_mday += dayOffset;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = 0;
  value.tm_isdst = -1;
  return mktime(&value);
}

time_t nextWeekdayTime(time_t now, int targetWeekday, int hour, int minute) {
  struct tm local {};
  if (!localtime_r(&now, &local)) return 0;
  int add = (targetWeekday - local.tm_wday + 7) % 7;
  time_t target = localTimeFor(now, hour, minute, add);
  if (target <= now) target = localTimeFor(now, hour, minute, add + 7);
  return target;
}

String naturalLabel(const String &text, bool timer) {
  for (const char *marker : {"提醒我", "叫我"}) {
    const int at = text.indexOf(marker);
    if (at >= 0) {
      String label = text.substring(at + strlen(marker));
      label.trim();
      if (!label.isEmpty()) return clipUtf8(label, 24);
    }
  }
  if (text.indexOf("起床") >= 0) return "起床";
  if (text.indexOf("吃药") >= 0) return "吃药";
  if (text.indexOf("开会") >= 0) return "开会";
  if (text.indexOf("休息") >= 0) return "休息";
  return timer ? "计时结束" : "语音闹钟";
}

size_t cancelMatchingTime(int hour, int minute) {
  size_t removed = 0;
  for (size_t i = 0; i < entryCount;) {
    bool match = false;
    if (entries[i].type == Type::Recurring) {
      match = entries[i].hour == hour && entries[i].minute == minute;
    } else if (entries[i].triggerAt >= CLOCK_READY_EPOCH) {
      const time_t value = static_cast<time_t>(entries[i].triggerAt);
      struct tm local {};
      localtime_r(&value, &local);
      match = local.tm_hour == hour && local.tm_min == minute;
    }
    if (!match) {
      ++i;
      continue;
    }
    for (size_t j = i + 1; j < entryCount; ++j) entries[j - 1] = entries[j];
    --entryCount;
    ++removed;
  }
  if (removed) persist();
  return removed;
}

bool removeNext(time_t now) {
  int selected = -1;
  time_t selectedAt = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    const time_t candidate = nextAt(entries[i], now);
    if (candidate && (!selectedAt || candidate < selectedAt)) {
      selectedAt = candidate;
      selected = static_cast<int>(i);
    }
  }
  return selected >= 0 && remove(entries[selected].id);
}

}  // namespace

bool begin() {
  Preferences storage;
  if (!storage.begin(NVS_NAMESPACE, true)) {
    // A fresh device has no namespace yet. Create it once so an empty alarm
    // list is a valid initial state rather than a boot error.
    if (!storage.begin(NVS_NAMESPACE, false)) return false;
    storage.putUInt(NVS_NEXT_ID_KEY, 1);
  }
  const String payload = storage.getString(NVS_ENTRIES_KEY, "");
  nextId = storage.getUInt(NVS_NEXT_ID_KEY, 1);
  if (nextId < 1) nextId = 1;
  storage.end();
  entryCount = 0;
  activeRing = false;
  if (payload.isEmpty()) return true;
  JsonDocument doc;
  if (deserializeJson(doc, payload)) return false;
  for (JsonObject item : doc["alarms"].as<JsonArray>()) {
    if (entryCount >= MAX_ENTRIES) break;
    Entry entry;
    entry.id = item["id"] | 0U;
    const uint8_t type = item["type"] | 0;
    if (!entry.id || type > static_cast<uint8_t>(Type::Timer)) continue;
    entry.type = static_cast<Type>(type);
    entry.enabled = item["enabled"] | true;
    entry.hour = item["hour"] | 0;
    entry.minute = item["minute"] | 0;
    entry.weekdays = item["weekdays"] | 0;
    entry.triggerAt = item["trigger_at"] | 0LL;
    entry.lastFiredAt = item["last_fired_at"] | 0LL;
    entry.label = clipUtf8(item["label"].as<String>(), 72);
    if (entry.hour > 23 || entry.minute > 59) continue;
    entries[entryCount++] = entry;
    if (nextId < entry.id + 1) nextId = entry.id + 1;
  }
  return true;
}

size_t count() { return entryCount; }

bool get(size_t index, Entry &entry) {
  if (index >= entryCount) return false;
  entry = entries[index];
  return true;
}

bool addRecurring(uint8_t hour, uint8_t minute, uint8_t weekdays,
                  const String &label, uint32_t *id) {
  if (hour > 23 || minute > 59 || !weekdays) return false;
  Entry entry;
  entry.type = Type::Recurring;
  entry.hour = hour;
  entry.minute = minute;
  entry.weekdays = weekdays & 0x7F;
  entry.label = label;
  return appendEntry(entry, id);
}

bool addOneShot(time_t triggerAt, const String &label, uint32_t *id) {
  if (triggerAt < CLOCK_READY_EPOCH) return false;
  Entry entry;
  entry.type = Type::OneShot;
  entry.triggerAt = triggerAt;
  entry.label = label;
  return appendEntry(entry, id);
}

bool addTimer(uint32_t durationSeconds, const String &label, time_t now, uint32_t *id) {
  if (now < CLOCK_READY_EPOCH || durationSeconds == 0 || durationSeconds > 7UL * 24UL * 3600UL) return false;
  Entry entry;
  entry.type = Type::Timer;
  entry.triggerAt = static_cast<int64_t>(now) + durationSeconds;
  entry.label = label;
  return appendEntry(entry, id);
}

bool remove(uint32_t id) {
  const int index = indexForId(id);
  if (index < 0) return false;
  for (size_t i = index + 1; i < entryCount; ++i) entries[i - 1] = entries[i];
  --entryCount;
  if (activeRing && activeEntry.id == id) activeRing = false;
  return persist();
}

bool setEnabled(uint32_t id, bool enabled) {
  const int index = indexForId(id);
  if (index < 0) return false;
  entries[index].enabled = enabled;
  return persist();
}

size_t clearAll() {
  const size_t removed = entryCount;
  entryCount = 0;
  activeRing = false;
  persist();
  return removed;
}

size_t cancelType(Type type) {
  size_t removed = 0;
  for (size_t i = 0; i < entryCount;) {
    if (entries[i].type != type) {
      ++i;
      continue;
    }
    for (size_t j = i + 1; j < entryCount; ++j) entries[j - 1] = entries[j];
    --entryCount;
    ++removed;
  }
  if (removed) persist();
  return removed;
}

bool poll(time_t now, Entry &fired) {
  if (activeRing || now < CLOCK_READY_EPOCH) return false;
  for (size_t i = 0; i < entryCount; ++i) {
    Entry &entry = entries[i];
    if (!entry.enabled) continue;
    bool due = false;
    if (entry.type == Type::Recurring) {
      struct tm local {};
      localtime_r(&now, &local);
      const int64_t currentMinute = static_cast<int64_t>(now) / 60;
      due = (entry.weekdays & (1U << local.tm_wday)) && local.tm_hour == entry.hour &&
            local.tm_min == entry.minute && entry.lastFiredAt / 60 != currentMinute;
    } else if (entry.triggerAt > 0 && now >= entry.triggerAt) {
      if (now - entry.triggerAt <= MISSED_ALARM_GRACE_SECONDS) due = true;
      else {
        entry.enabled = false;
        persist();
      }
    }
    if (!due) continue;
    entry.lastFiredAt = now;
    activeEntry = entry;
    activeRing = true;
    if (entry.type != Type::Recurring) entry.enabled = false;
    persist();
    fired = activeEntry;
    return true;
  }
  return false;
}

bool ringing() { return activeRing; }

Entry ringingEntry() { return activeEntry; }

void dismiss() {
  const uint32_t completedId = activeEntry.id;
  const bool removeCompleted = activeEntry.type != Type::Recurring;
  activeRing = false;
  if (removeCompleted) remove(completedId);
}

bool snooze(time_t now, uint32_t seconds) {
  if (!activeRing || !seconds || seconds > 7200) return false;
  const String label = String("稍后提醒：") + activeEntry.label;
  const uint32_t completedId = activeEntry.id;
  const bool removeCompleted = activeEntry.type != Type::Recurring;
  if (removeCompleted) {
    for (size_t i = 0; i < entryCount; ++i) {
      if (entries[i].id != completedId) continue;
      const Entry previous = entries[i];
      entries[i].type = Type::OneShot;
      entries[i].enabled = true;
      entries[i].triggerAt = now + seconds;
      entries[i].lastFiredAt = 0;
      entries[i].label = label;
      if (!persist()) { entries[i] = previous; return false; }
      activeRing = false;
      return true;
    }
  }
  if (!addOneShot(now + seconds, label)) return false;
  activeRing = false;
  return true;
}

String toJson(time_t now) {
  JsonDocument doc;
  doc["count"] = entryCount;
  doc["ringing"] = activeRing;
  if (activeRing) {
    doc["ringing_id"] = activeEntry.id;
    doc["ringing_label"] = activeEntry.label;
  }
  JsonArray list = doc["alarms"].to<JsonArray>();
  for (size_t i = 0; i < entryCount; ++i) {
    const Entry &entry = entries[i];
    JsonObject item = list.add<JsonObject>();
    item["id"] = entry.id;
    item["type"] = entry.type == Type::Recurring ? "recurring" : entry.type == Type::Timer ? "timer" : "once";
    item["enabled"] = entry.enabled;
    item["label"] = entry.label;
    item["hour"] = entry.hour;
    item["minute"] = entry.minute;
    item["weekdays"] = entry.weekdays;
    item["schedule"] = entry.type == Type::Recurring
                           ? weekdayDescription(entry.weekdays) + " " + formatClock(entry.hour, entry.minute)
                           : formatLocalTime(static_cast<time_t>(entry.triggerAt));
    const time_t next = nextAt(entry, now);
    item["next_at"] = static_cast<int64_t>(next);
    item["remaining_seconds"] = next > now ? static_cast<int64_t>(next - now) : 0;
  }
  String output;
  serializeJson(doc, output);
  return output;
}

String summary(time_t now) {
  size_t enabled = 0;
  for (size_t i = 0; i < entryCount; ++i) enabled += entries[i].enabled ? 1 : 0;
  if (!enabled) return "当前没有启用的闹钟或计时器";
  return "共" + String(enabled) + "个已启用，" + nextSummary(now);
}

String nextSummary(time_t now) {
  const Entry *selected = nullptr;
  time_t selectedAt = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    const time_t candidate = nextAt(entries[i], now);
    if (candidate && (!selectedAt || candidate < selectedAt)) {
      selected = &entries[i];
      selectedAt = candidate;
    }
  }
  if (!selected) return "暂无闹钟";
  return "下次 " + formatLocalTime(selectedAt) + " " + selected->label;
}

CommandResult handleNaturalLanguage(const String &input, time_t now) {
  CommandResult result;
  if (now < CLOCK_READY_EPOCH) return result;
  const String text = compactText(input);
  // Ring commands take priority over scheduling a new timer.
  if (activeRing) {
    const bool stop = text.indexOf("停止闹钟") >= 0 || text.indexOf("结束闹钟") >= 0 ||
        text.indexOf("关闭闹钟") >= 0 || text == "别响了" || text == "停止" || text == "停一下";
    const bool later = text.indexOf("再等") >= 0 || text.indexOf("稍后") >= 0 ||
        text.indexOf("延后") >= 0 || text.indexOf("贪睡") >= 0 ||
        (text.indexOf("再") >= 0 && text.indexOf("分钟") >= 0);
    if (stop || later) {
      result.handled = true;
      if (stop) { dismiss(); result.changed = true; result.message = "闹钟已停止"; }
      else {
        const int minutes = findNumberWithUnit(text, "分钟", 120);
        const uint32_t seconds = (minutes > 0 ? minutes : 5) * 60UL;
        result.changed = snooze(now, seconds);
        result.message = result.changed ? "将在" + String(seconds / 60) + "分钟后再次提醒" : "稍后提醒保存失败";
      }
      return result;
    }
  }
  const bool mentionsAlarm = text.indexOf("闹钟") >= 0 || text.indexOf("提醒") >= 0 ||
                             text.indexOf("叫我") >= 0;
  const bool relativeReminder = text.indexOf("后") >= 0 &&
                                (text.indexOf("小时") >= 0 || text.indexOf("分钟") >= 0 ||
                                 text.indexOf("秒") >= 0);
  const bool mentionsTimer = text.indexOf("计时") >= 0 || text.indexOf("倒计时") >= 0 ||
                             relativeReminder;
  if (!mentionsAlarm && !mentionsTimer) return result;

  String actionText = text;
  for (const char *marker : {"提醒我", "叫我"}) {
    const int at = actionText.indexOf(marker);
    if (at >= 0) actionText = actionText.substring(0, at);
  }
  const bool cancel = actionText.indexOf("取消") >= 0 || actionText.indexOf("删除") >= 0 ||
                      actionText.indexOf("关闭") >= 0 || actionText.indexOf("清空") >= 0;
  const bool query = actionText.indexOf("查看") >= 0 || actionText.indexOf("哪些") >= 0 ||
                     actionText.indexOf("所有") >= 0 || actionText.indexOf("列表") >= 0 ||
                     actionText.indexOf("还有") >= 0;
  if (cancel) {
    result.handled = true;
    size_t removed = 0;
    if (text.indexOf("所有") >= 0 || text.indexOf("全部") >= 0 || text.indexOf("清空") >= 0) {
      removed = clearAll();
    } else if (mentionsTimer && !mentionsAlarm) {
      removed = cancelType(Type::Timer);
    } else {
      int hour = -1;
      int minute = 0;
      if (parseClockTime(text, hour, minute)) removed = cancelMatchingTime(hour, minute);
      else removed = removeNext(now) ? 1 : 0;
    }
    result.changed = removed > 0;
    result.message = removed ? "已取消" + String(removed) + "个闹钟或计时器" : "没有找到匹配的闹钟";
    return result;
  }
  if (query) {
    result.handled = true;
    result.message = summary(now);
    return result;
  }

  if (mentionsTimer) {
    const int hours = max(0, findNumberWithUnit(text, "小时", 168));
    const int minutes = max(0, findNumberWithUnit(text, "分钟", 10080));
    const int seconds = max(0, findNumberWithUnit(text, "秒", 604800));
    const uint32_t duration = static_cast<uint32_t>(hours) * 3600UL +
                              static_cast<uint32_t>(minutes) * 60UL + seconds;
    if (!duration) return result;
    result.handled = true;
    result.changed = addTimer(duration, naturalLabel(text, true), now);
    result.message = result.changed ? "已开始" + String(duration) + "秒计时" : "计时器保存失败或数量已满";
    return result;
  }

  int hour = -1;
  int minute = 0;
  if (!parseClockTime(text, hour, minute)) return result;
  result.handled = true;
  const String label = naturalLabel(text, false);
  const uint8_t weekdays = weekdayMaskFromText(text);
  const bool explicitlyRecurring = text.indexOf("每天") >= 0 || text.indexOf("每日") >= 0 ||
                                   text.indexOf("工作日") >= 0 || text.indexOf("周末") >= 0 ||
                                   text.indexOf("每周") >= 0 || text.indexOf("每星期") >= 0;
  uint32_t id = 0;
  if (explicitlyRecurring) {
    result.changed = addRecurring(hour, minute, weekdays ? weekdays : 0x7F, label, &id);
    result.message = result.changed ? "已设置" + weekdayDescription(weekdays ? weekdays : 0x7F) +
                                       formatClock(hour, minute) + "的闹钟"
                                    : "闹钟保存失败或数量已满";
    return result;
  }

  time_t triggerAt = 0;
  if (text.indexOf("后天") >= 0) triggerAt = localTimeFor(now, hour, minute, 2);
  else if (text.indexOf("明天") >= 0) triggerAt = localTimeFor(now, hour, minute, 1);
  else if (weekdays) {
    for (uint8_t day = 0; day < 7; ++day) {
      if (weekdays & (1U << day)) {
        const time_t candidate = nextWeekdayTime(now, day, hour, minute);
        if (!triggerAt || candidate < triggerAt) triggerAt = candidate;
      }
    }
  } else {
    triggerAt = localTimeFor(now, hour, minute, 0);
    if (triggerAt <= now) triggerAt = localTimeFor(now, hour, minute, 1);
  }
  result.changed = addOneShot(triggerAt, label, &id);
  result.message = result.changed ? "已设置" + formatLocalTime(triggerAt) + "的闹钟"
                                  : "闹钟保存失败或数量已满";
  return result;
}

}  // namespace AlarmManager
