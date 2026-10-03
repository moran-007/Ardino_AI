#pragma once
#include <Arduino.h>
namespace MusicVoice {
inline String chineseNumber(int value) {
  const char *digits[] = {"零", "一", "二", "三", "四", "五", "六", "七", "八", "九"};
  if (value == 100) return "一百";
  if (value < 10) return digits[value];
  return (value < 20 ? String() : String(digits[value / 10])) + "十" + (value % 10 ? digits[value % 10] : "");
}
// Explicit volume commands only; callers reject notes/reminders beforehand.
inline int volume(const String &text, int current) {
  if (text.indexOf("音量") < 0) return -1;
  if (text.indexOf("调大") >= 0 || text.indexOf("大一点") >= 0 || text.indexOf("提高") >= 0 || text.indexOf("增大") >= 0) return min(100, current + 10);
  if (text.indexOf("调小") >= 0 || text.indexOf("小一点") >= 0 || text.indexOf("降低") >= 0 || text.indexOf("减小") >= 0) return max(0, current - 10);
  if (text.indexOf("最大") >= 0) return 100;
  if (text.indexOf("最小") >= 0 || text.indexOf("静音") >= 0) return 0;
  int start = text.indexOf("百分之");
  if (start >= 0) start += strlen("百分之");
  else { start = text.indexOf("到"); if (start >= 0) start += strlen("到"); else start = text.indexOf("音量") + strlen("音量"); }
  const String tail = text.substring(start);
  for (size_t i = 0; i < tail.length(); ++i) {
    if (tail[i] >= '0' && tail[i] <= '9') {
      unsigned result = 0; size_t count = 0;
      while (i < tail.length() && tail[i] >= '0' && tail[i] <= '9') { if (++count > 3) return -1; result = result * 10 + tail[i++] - '0'; }
      return min(100, static_cast<int>(result));
    }
    const String suffix = tail.substring(i);
    for (int n = 100; n >= 0; --n) if (suffix.startsWith(chineseNumber(n))) return n;
  }
  return -1;
}
}
