#pragma once

#include <Arduino.h>

namespace RobotExpressions {
// Stable IDs: persisted masks and web clients refer to the row index.
enum class Eyes : uint8_t {
  Open, Happy, Wink, Heart, Star, Look, Question, Sleep, Half, Closed,
  Tear, Sad, Angry, Side, Wide, Uneven, Spiral, Cross, Squeeze, Money,
};
enum class Mouth : uint8_t { Smile, Laugh, Small, Flat, Yawn, Frown, Wobble, Pout, Tongue, Grin, Kiss, Open };
enum class Gesture : uint8_t { Wave, Cheer, Clap, Thumbs, Heart, Hug, Shy, Think, Point, Shrug, Rest, Stretch, Wipe, CrossArms, Shake, Surprise, Dance, Peace };
enum class Accent : uint8_t { None, Sparkle, Hearts, Zzz, Tears, Sweat, Anger, Question, Blush, Confetti };
struct Expression {
  const char *name;
  uint8_t category;
  Eyes eyes;
  Mouth mouth;
  Gesture gesture;
  Accent accent;
  uint16_t color;
};
constexpr uint8_t CATEGORY_COUNT = 8;
constexpr uint8_t COUNT = 48;
constexpr uint64_t ALL = (uint64_t{1} << COUNT) - 1;
extern const char *const CATEGORIES[CATEGORY_COUNT];
extern const Expression CATALOG[COUNT];
extern const char *const GESTURE_LABELS[18];
inline const Expression &get(uint8_t id) { return CATALOG[id < COUNT ? id : 0]; }
inline bool validMask(uint64_t mask) { return mask != 0 && (mask & ~ALL) == 0; }
// Uniformly select an enabled item; avoid immediate repeats when possible.
uint8_t choose(uint64_t mask, uint8_t previous, uint32_t entropy);
// Strict CSV of stable IDs. Reject duplicates, malformed entries and an empty set.
bool parseSelection(const String &csv, uint64_t &mask);
bool parseId(const String &value, uint8_t &id);
}  // namespace RobotExpressions
