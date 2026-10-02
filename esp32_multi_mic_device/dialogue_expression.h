#pragma once
#include <Arduino.h>
namespace DialogueExpression {
// Stable IDs in RobotExpressions::CATALOG; a disabled expression is skipped.
inline uint8_t choose(const String &question, const String &answer) {
  if (question.indexOf("难过") >= 0 || question.indexOf("伤心") >= 0 ||
      question.indexOf("孤独") >= 0 || answer.indexOf("抱抱") >= 0) return 5;
  if (question.indexOf("睡觉") >= 0 || question.indexOf("晚安") >= 0) return 22;
  if (question == "你好" || question == "早上好" || question == "你好呀") return 0;
  if (answer.indexOf("恭喜") >= 0 || question.indexOf("成功了") >= 0) return 45;
  if (question.indexOf("谢谢") >= 0 || answer.indexOf("加油") >= 0) return 4;
  return 15;
}
}
