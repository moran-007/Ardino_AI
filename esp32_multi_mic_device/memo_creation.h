#pragma once

#include "memo_manager.h"

namespace MemoCreation {
struct Intent {
  bool handled = false;
  MemoManager::Kind kind = MemoManager::Kind::Note;
  bool reminderAlso = false;
  String content;
};

inline String trimPunctuation(String text) {
  text.trim();
  static const char *marks[] = {",", "，", "。", ".", ":", "：", "!", "！"};
  bool changed;
  do {
    changed = false;
    for (const char *mark : marks) {
      const size_t length = strlen(mark);
      if (text.startsWith(mark)) { text.remove(0, length); changed = true; }
      if (text.endsWith(mark)) { text.remove(text.length() - length); changed = true; }
    }
    text.trim();
  } while (changed && !text.isEmpty());
  return text;
}

inline Intent parse(String text) {
  text = trimPunctuation(text);
  text.replace("代办", "待办");
  for (const char *polite : {"请", "帮我", "给我"}) {
    if (text.startsWith(polite)) text = trimPunctuation(text.substring(strlen(polite)));
  }
  static const char *todo[] = {"添加待办", "新增待办", "创建待办", "加入待办", "记个待办",
                             "记一条待办", "加个待办", "加一个待办", "加一条待办", "添加到待办"};
  static const char *notes[] = {"记一下", "记下来", "记下", "添加便签", "新增便签", "创建便签", "写个便签"};
  Intent result;
  for (const char *prefix : todo) {
    if (!text.startsWith(prefix)) continue;
    result.handled = true;
    result.kind = MemoManager::Kind::Todo;
    result.content = trimPunctuation(text.substring(strlen(prefix)));
    return result;
  }
  for (const char *suffix : todo) {
    if (!text.endsWith(suffix)) continue;
    result.handled = true;
    result.kind = MemoManager::Kind::Todo;
    result.content = trimPunctuation(text.substring(0, text.length() - strlen(suffix)));
    if (result.content.startsWith("把")) result.content = trimPunctuation(result.content.substring(strlen("把")));
    result.reminderAlso = result.content.indexOf("提醒我") >= 0 || result.content.indexOf("叫我") >= 0;
    return result;
  }
  for (const char *prefix : notes) {
    if (!text.startsWith(prefix)) continue;
    result.handled = true;
    result.content = trimPunctuation(text.substring(strlen(prefix)));
    return result;
  }
  if (text.endsWith("记到便签")) {
    result.handled = true;
    result.content = trimPunctuation(text.substring(0, text.length() - strlen("记到便签")));
    if (result.content.startsWith("把")) result.content = trimPunctuation(result.content.substring(strlen("把")));
  }
  return result;
}
}  // namespace MemoCreation
