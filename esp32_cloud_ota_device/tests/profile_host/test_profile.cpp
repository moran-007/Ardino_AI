#include "personal_profile.h"
#include <cassert>
#include <iostream>
int main() {
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  fs::FS storage;
  memoFiles.clear();
  assert(PersonalProfile::begin(storage));
  assert(PersonalProfile::observe("我叫小明").changed);
  assert(PersonalProfile::observe("我喜欢机器人").changed);
  assert(!PersonalProfile::observe("假如我叫小王").changed);
  assert(!PersonalProfile::observe("我喜欢银行卡密码1234").changed);
  assert(!PersonalProfile::observe("我喜欢机器人").changed);
  const String beforeInvalidDelete = PersonalProfile::toJson();
  assert(!PersonalProfile::handleNaturalLanguage("忘记个人记忆1不要删除").changed);
  assert(!PersonalProfile::handleNaturalLanguage("忘记个人记忆4294967297号").changed);
  assert(PersonalProfile::toJson() == beforeInvalidDelete);
  const auto observedRevision = PersonalProfile::revision();
  assert(PersonalProfile::handleNaturalLanguage("记住我叫小明").changed);
  assert(PersonalProfile::revision() > observedRevision);
  assert(!PersonalProfile::observe("我叫小明").changed);
  const auto rev = PersonalProfile::revision();
  assert(PersonalProfile::begin(storage));
  assert(PersonalProfile::revision() == rev);
  assert(PersonalProfile::toJson().indexOf("小明") >= 0);
  auto result = PersonalProfile::handleNaturalLanguage("忘记我的名字");
  assert(result.handled && result.changed);
  assert(PersonalProfile::toJson().indexOf("小明") < 0);
  assert(PersonalProfile::revision() > rev);
  failMemoWrite = true;
  assert(!PersonalProfile::observe("我叫小王").changed);
  assert(PersonalProfile::toJson().indexOf("小王") < 0);
  failMemoWrite = false;
  assert(PersonalProfile::setPolicy(false, false, false));
  assert(!PersonalProfile::observe("我喜欢音乐").changed);
  assert(PersonalProfile::clear());
  assert(PersonalProfile::begin(storage));
  assert(PersonalProfile::toJson().indexOf("机器人") < 0);
  *memoFiles["/personal_profile.json"] = "broken";
  assert(!PersonalProfile::begin(storage));
  assert(!PersonalProfile::ready());
  assert(!PersonalProfile::observe("我叫误覆盖").changed);
  JsonDocument backup;
  assert(!deserializeJson(backup, "{\"schema\":1,\"revision\":100,\"enabled\":true,\"auto_collect\":true,\"backup_enabled\":true,\"items\":[{\"id\":1,\"category\":\"name\",\"value\":\"恢复的称呼\",\"source\":\"explicit\"}]}"));
  assert(PersonalProfile::restore(backup.as<JsonVariantConst>()));
  assert(PersonalProfile::ready() && PersonalProfile::revision() == 101);
  failProfileBackupRemove = true;
  assert(!PersonalProfile::clear());
  assert(PersonalProfile::toJson().indexOf("恢复的称呼") < 0);
  assert(memoFiles.count("/personal_profile.bak") == 1);
  assert(String(PersonalProfile::lastError()).indexOf("旧副本") >= 0);
  assert(PersonalProfile::begin(storage));
  assert(PersonalProfile::toJson().indexOf("恢复的称呼") < 0);
  assert(!PersonalProfile::setPolicy(true, true, true));
  failProfileBackupRemove = false;
  const auto cleanupRevision = PersonalProfile::revision();
  assert(PersonalProfile::setPolicy(true, true, true));
  assert(PersonalProfile::revision() == cleanupRevision);
  assert(memoFiles.count("/personal_profile.bak") == 0);
  memoFiles["/personal_profile.bak"] = std::make_shared<std::string>("damaged old backup");
  *memoFiles["/personal_profile.json"] = "broken";
  assert(!PersonalProfile::begin(storage));
  assert(PersonalProfile::toJson().indexOf("恢复的称呼") < 0);
  failProfileBackupRemove = false;
  assert(PersonalProfile::restore(backup.as<JsonVariantConst>()));
  assert(PersonalProfile::clear());
  assert(memoFiles.count("/personal_profile.bak") == 0);
  std::cout << "Personal profile behavior checks passed\n";
}
