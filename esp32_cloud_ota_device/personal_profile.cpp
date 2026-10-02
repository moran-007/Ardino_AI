#include "personal_profile.h"

namespace PersonalProfile {
namespace {
constexpr char PATH[] = "/personal_profile.json";
constexpr char TEMP[] = "/personal_profile.tmp";
constexpr char BACKUP[] = "/personal_profile.bak";
struct Item { uint32_t id = 0; String category, value, source; };
struct Profile {
  uint32_t revision = 0;
  bool enabled = true, autoCollect = true, backupEnabled = true;
  size_t count = 0;
  Item items[MAX_ITEMS];
};
Profile state;
fs::FS *storage = nullptr;
bool initialized = false;
String errorText;

bool categoryValid(const String &value) {
  return value == "name" || value == "interest" || value == "communication" ||
         value == "goal" || value == "habit";
}
String clean(String value) {
  value.trim();
  value.replace("\r", " "); value.replace("\n", " ");
  while (value.endsWith("。") || value.endsWith("！") || value.endsWith("，"))
    value.remove(value.length() - 3);
  value.trim();
  return value;
}
bool acceptable(const String &value) {
  if (value.isEmpty() || value.length() > MAX_VALUE_BYTES) return false;
  for (size_t i = 0; i < value.length(); ++i)
    if (static_cast<uint8_t>(value[i]) < 32U) return false;
  const char *sensitive[] = {"身份证", "银行卡", "密码", "密钥", "手机号", "电话",
      "住址", "地址", "病史", "疾病", "诊断", "账户", "账号", "信用卡", "API", "sk-",
      "注射", "服用", "胰岛素", "药", "治疗", "医疗", "抑郁"};
  for (const char *word : sensitive) if (value.indexOf(word) >= 0) return false;
  // Do not automatically retain long numeric identifiers embedded in a fact.
  size_t digits = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    digits = value[i] >= '0' && value[i] <= '9' ? digits + 1 : 0;
    if (digits >= 7) return false;
  }
  return true;
}
void encode(const Profile &value, JsonObject root) {
  root["schema"] = 1;
  root["revision"] = value.revision;
  root["enabled"] = value.enabled;
  root["auto_collect"] = value.autoCollect;
  root["backup_enabled"] = value.backupEnabled;
  JsonArray items = root["items"].to<JsonArray>();
  for (size_t i = 0; i < value.count; ++i) {
    JsonObject item = items.add<JsonObject>();
    item["id"] = value.items[i].id;
    item["category"] = value.items[i].category;
    item["value"] = value.items[i].value;
    item["source"] = value.items[i].source;
  }
}
bool decode(JsonVariantConst root, Profile &value) {
  if (!root.is<JsonObjectConst>() || !root["schema"].is<int>() || root["schema"].as<int>() != 1 ||
      !root["revision"].is<uint32_t>() || !root["enabled"].is<bool>() ||
      !root["auto_collect"].is<bool>() || !root["backup_enabled"].is<bool>() ||
      !root["items"].is<JsonArrayConst>()) return false;
  value.revision = root["revision"].as<uint32_t>();
  value.enabled = root["enabled"].as<bool>();
  value.autoCollect = root["auto_collect"].as<bool>();
  value.backupEnabled = root["backup_enabled"].as<bool>();
  value.count = 0;
  for (JsonObjectConst item : root["items"].as<JsonArrayConst>()) {
    if (value.count >= MAX_ITEMS || !item["id"].is<uint32_t>() ||
        !item["category"].is<const char *>() || !item["value"].is<const char *>() ||
        !item["source"].is<const char *>()) return false;
    Item &next = value.items[value.count];
    next.id = item["id"].as<uint32_t>();
    next.category = item["category"].as<String>();
    next.value = item["value"].as<String>();
    next.source = item["source"].as<String>();
    if (!next.id || !categoryValid(next.category) || !acceptable(next.value) ||
        (next.source != "explicit" && next.source != "observed")) return false;
    for (size_t i = 0; i < value.count; ++i) if (value.items[i].id == next.id) return false;
    ++value.count;
  }
  return true;
}
bool read(const char *path, Profile &value) {
  File file = storage->open(path, FILE_READ);
  if (!file) return false;
  JsonDocument doc;
  const bool ok = !deserializeJson(doc, file) && decode(doc.as<JsonVariantConst>(), value);
  file.close();
  return ok;
}
bool finishUnchangedSave() {
  if (!initialized || !storage) { errorText = "个人记忆存储未就绪"; return false; }
  if (storage->exists(BACKUP) && !storage->remove(BACKUP)) {
    errorText = "当前个人记忆已保留，但旧副本清理失败；请再次保存或清空以重试";
    return false;
  }
  errorText = "";
  return true;
}
bool commit(Profile &next) {
  if (!initialized || !storage) { errorText = "个人记忆存储未就绪"; return false; }
  if (state.revision == UINT32_MAX) { errorText = "个人记忆版本已达上限"; return false; }
  next.revision = max(state.revision, next.revision);
  if (next.revision == UINT32_MAX) { errorText = "个人记忆版本已达上限"; return false; }
  ++next.revision;
  JsonDocument doc;
  encode(next, doc.to<JsonObject>());
  storage->remove(TEMP);
  File file = storage->open(TEMP, FILE_WRITE);
  const size_t expected = measureJson(doc);
  const size_t written = file ? serializeJson(doc, file) : 0;
  file.flush(); file.close();
  if (written != expected || written == 0) {
    storage->remove(TEMP); errorText = "个人记忆写入失败"; return false;
  }
  // Read back before rotating so partial or corrupt writes never replace primary.
  Profile verified;
  if (!read(TEMP, verified)) { storage->remove(TEMP); errorText = "个人记忆校验失败"; return false; }
  if (storage->exists(BACKUP) && !storage->remove(BACKUP)) {
    errorText = "个人记忆备份清理失败"; return false;
  }
  const bool hadPrimary = storage->exists(PATH);
  if (hadPrimary && !storage->rename(PATH, BACKUP)) { errorText = "个人记忆替换失败"; return false; }
  if (!storage->rename(TEMP, PATH)) {
    if (hadPrimary) storage->rename(BACKUP, PATH);
    errorText = "个人记忆保存失败"; return false;
  }
  state = next;
  if (storage->exists(BACKUP) && !storage->remove(BACKUP)) {
    errorText = "当前个人记忆已更新，但旧副本清理失败；请再次保存或清空以重试";
    return false;
  }
  errorText = "";
  return true;
}
const char *label(const String &category) {
  if (category == "name") return "称呼";
  if (category == "interest") return "兴趣";
  if (category == "communication") return "交流偏好";
  if (category == "goal") return "目标";
  return "习惯";
}
CommandResult collect(const String &text, bool explicitRequest) {
  struct Prefix { const char *text, *category; };
  const Prefix prefixes[] = {{"我叫", "name"}, {"我的名字是", "name"}, {"请叫我", "name"},
      {"我喜欢", "interest"}, {"我不喜欢", "interest"}, {"我正在学习", "goal"},
      {"我想学习", "goal"}, {"我的目标是", "goal"}, {"我习惯", "habit"},
      {"我每天", "habit"}, {"我希望你", "communication"}, {"我希望回答", "communication"}};
  CommandResult result;
  for (const auto &prefix : prefixes) {
    if (!text.startsWith(prefix.text)) continue;
    String value = clean(text.substring(strlen(prefix.text)));
    if (String(prefix.text) == "我不喜欢") value = "不喜欢" + value;
    if (String(prefix.text) == "我每天") value = "每天" + value;
    result.handled = explicitRequest;
    const uint32_t previousRevision = state.revision;
    if (add(prefix.category, value, explicitRequest ? "explicit" : "observed")) {
      result.changed = state.revision != previousRevision;
      result.message = "已记住" + String(label(prefix.category)) + "：" + value + "。";
    } else if (explicitRequest) result.message = String("未保存：") + lastError();
    return result;
  }
  if (explicitRequest) {
    result.handled = true;
    result.message = "请明确告诉我称呼、兴趣、交流偏好、学习目标或习惯，例如：记住我喜欢机器人。";
  }
  return result;
}
}

bool begin(fs::FS &target) {
  storage = &target; initialized = false; state = Profile(); errorText = "";
  Profile loaded;
  if (storage->exists(PATH)) {
    // Corrupt primary must not silently restore deleted information from an old backup.
    if (!read(PATH, loaded)) { errorText = "个人记忆文件损坏，请手动恢复备份"; return false; }
    state = loaded;
  } else if (storage->exists(BACKUP)) {
    // A backup with no primary represents interrupted rotation.
    if (!read(BACKUP, loaded) || !storage->rename(BACKUP, PATH)) {
      errorText = "个人记忆恢复失败"; return false;
    }
    state = loaded;
  }
  initialized = true;
  if (storage->exists(BACKUP)) errorText = "当前个人记忆已读取；仍有旧副本，请保存或清空以重试清理";
  return true;
}
bool ready() { return initialized; }
uint32_t revision() { return state.revision; }
String toJson() { JsonDocument doc; encode(state, doc.to<JsonObject>()); String out; serializeJson(doc, out); return out; }
void appendSnapshot(JsonObject root) { encode(state, root); }
const char *lastError() { return errorText.c_str(); }
bool setPolicy(bool enabled, bool autoCollect, bool backupEnabled) {
  if (state.enabled == enabled && state.autoCollect == autoCollect && state.backupEnabled == backupEnabled)
    return finishUnchangedSave();
  Profile next = state;
  next.enabled = enabled; next.autoCollect = autoCollect; next.backupEnabled = backupEnabled;
  return commit(next);
}
bool add(const String &category, const String &text, const String &source) {
  const String value = clean(text);
  if (!categoryValid(category) || !acceptable(value) || (source != "explicit" && source != "observed")) {
    errorText = "内容为空、过长或包含不收集的信息"; return false;
  }
  if (!state.enabled) { errorText = "个人记忆已关闭"; return false; }
  Profile next = state;
  uint32_t nextId = 1;
  int replaceIndex = -1;
  for (size_t i = 0; i < state.count; ++i) {
    if (state.items[i].id == UINT32_MAX) { errorText = "记忆编号已达上限"; return false; }
    nextId = max(nextId, state.items[i].id + 1);
    if (state.items[i].category == category && state.items[i].value == value) {
      if (source == "explicit" && state.items[i].source == "observed") {
        next.items[i].source = "explicit"; return commit(next);
      }
      if (source == "explicit") return finishUnchangedSave();
      errorText = "已记住这条信息"; return false;
    }
    if (category == "name" && state.items[i].category == "name") replaceIndex = static_cast<int>(i);
  }
  if (replaceIndex < 0 && next.count >= MAX_ITEMS) { errorText = "记忆已满，请删除不再需要的信息"; return false; }
  Item &item = next.items[replaceIndex >= 0 ? static_cast<size_t>(replaceIndex) : next.count++];
  if (replaceIndex < 0) item.id = nextId;
  item.category = category; item.value = value; item.source = source;
  return commit(next);
}
bool remove(uint32_t id) {
  Profile next = state;
  for (size_t i = 0; i < next.count; ++i) if (next.items[i].id == id) {
    for (size_t j = i + 1; j < next.count; ++j) next.items[j - 1] = next.items[j];
    --next.count; return commit(next);
  }
  errorText = "没有这条个人记忆"; return false;
}
bool clear() { Profile next = state; next.count = 0; return commit(next); }
bool edit(uint32_t id, const String &text) {
  const String value = clean(text);
  if (!acceptable(value)) { errorText = "内容为空、过长或包含不收集的信息"; return false; }
  Profile next = state;
  for (size_t i = 0; i < next.count; ++i) if (next.items[i].id == id) {
    if (next.items[i].value == value && next.items[i].source == "explicit") return finishUnchangedSave();
    next.items[i].value = value; next.items[i].source = "explicit"; return commit(next);
  }
  errorText = "没有这条个人记忆"; return false;
}
bool restore(JsonVariantConst snapshot) {
  Profile next;
  if (!decode(snapshot, next)) { errorText = "备份格式无效"; return false; }
  if (!storage) { errorText = "个人记忆存储未就绪"; return false; }
  const bool wasInitialized = initialized;
  initialized = true;  // Only this explicit, validated recovery may replace corrupt primary.
  if (commit(next)) return true;
  initialized = wasInitialized;
  return false;
}
CommandResult observe(const String &text) {
  if (!initialized || !state.enabled || !state.autoCollect) return CommandResult();
  return collect(clean(text), false);
}
CommandResult handleNaturalLanguage(const String &input) {
  String text = clean(input);
  CommandResult result;
  if (text == "你记住了什么" || text == "我的个人信息" || text == "查看个人记忆") {
    result.handled = true;
    if (!initialized) { result.message = lastError(); return result; }
    result.message = state.count ? "我记住了：" : "还没有保存个人信息。";
    for (size_t i = 0; i < state.count; ++i) {
      if (i) result.message += "；";
      result.message += String(state.items[i].id) + "号" + label(state.items[i].category) + "：" + state.items[i].value;
      // Keep spoken command result within the cloud 600-character bound.
      if (i >= 2) { if (state.count > 3) result.message += "；更多请在个人记忆页查看。"; break; }
    }
    return result;
  }
  if (text == "清空个人记忆" || text == "忘记我的所有信息") {
    result.handled = true; result.changed = clear();
    result.message = result.changed ? "个人记忆已清空，云端备份将在联网后同步清除。" : lastError();
    return result;
  }
  if (text == "关闭个人记忆" || text == "开启个人记忆" || text == "停止收集个人信息" || text == "开启自动记忆") {
    result.handled = true;
    const bool enabled = text == "开启个人记忆" || text == "开启自动记忆" ? true :
                         text == "关闭个人记忆" ? false : state.enabled;
    const bool automatic = text == "停止收集个人信息" ? false : text == "开启自动记忆" ? true : state.autoCollect;
    result.changed = setPolicy(enabled, automatic, state.backupEnabled);
    result.message = result.changed ? "个人记忆偏好已保存。" : lastError(); return result;
  }
  if (text == "忘记我的名字" || text == "忘记我的称呼") {
    result.handled = true;
    for (size_t i = 0; i < state.count; ++i) if (state.items[i].category == "name") {
      result.changed = remove(state.items[i].id); result.message = result.changed ? "已忘记你的称呼。" : lastError(); return result;
    }
    result.message = "没有保存你的称呼。"; return result;
  }
  if (text.startsWith("忘记个人记忆")) {
    result.handled = true;
    String number = text.substring(strlen("忘记个人记忆"));
    if (number.endsWith("号")) number.remove(number.length() - strlen("号"));
    uint32_t id = 0;
    bool valid = !number.isEmpty();
    for (size_t i = 0; valid && i < number.length(); ++i) {
      if (number[i] < '0' || number[i] > '9' || id > (UINT32_MAX - (number[i] - '0')) / 10U) valid = false;
      else id = id * 10U + static_cast<uint32_t>(number[i] - '0');
    }
    if (!valid || id == 0) { result.message = "请说完整的记忆编号，例如：忘记个人记忆1号。"; return result; }
    result.changed = remove(id); result.message = result.changed ? "已忘记这条个人记忆。" : lastError(); return result;
  }
  if (text.startsWith("记住")) return collect(clean(text.substring(strlen("记住"))), true);
  return result;
}
}
