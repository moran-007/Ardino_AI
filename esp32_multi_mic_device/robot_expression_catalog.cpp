#include "robot_expression_catalog.h"

namespace RobotExpressions {
const char *const CATEGORIES[CATEGORY_COUNT] = {
  "开心互动", "喜欢害羞", "好奇思考", "困倦放松",
  "委屈难过", "生气傲娇", "惊讶紧张", "搞怪庆祝",
};
const char *const GESTURE_LABELS[18] = {
  "挥手", "举手欢呼", "拍手", "点赞", "比心", "拥抱", "捂脸", "托腮",
  "指向", "摊手", "休息", "伸懒腰", "擦泪", "抱臂", "握拳", "惊讶捂脸", "跳舞", "胜利手势",
};
using E = Eyes; using M = Mouth; using G = Gesture; using A = Accent;
constexpr uint16_t cyan=0x05FF, green=0x07E0, pink=0xFAD4, gold=0xFFE0;
constexpr uint16_t blue=0x64BF, purple=0xB2FF, orange=0xFC60, white=0xEF7D;
const Expression CATALOG[COUNT] = {
  {"微笑招手",0,E::Open,M::Smile,G::Wave,A::None,cyan},
  {"开心拍手",0,E::Happy,M::Laugh,G::Clap,A::Sparkle,green},
  {"俏皮眨眼",0,E::Wink,M::Smile,G::Peace,A::Blush,cyan},
  {"灿烂大笑",0,E::Squeeze,M::Laugh,G::Cheer,A::None,gold},
  {"赞一个",0,E::Half,M::Grin,G::Thumbs,A::Sparkle,green},
  {"欢迎拥抱",0,E::Happy,M::Smile,G::Hug,A::Hearts,cyan},
  {"爱心眼",1,E::Heart,M::Smile,G::Heart,A::Hearts,pink},
  {"害羞捂脸",1,E::Closed,M::Small,G::Shy,A::Blush,pink},
  {"飞吻",1,E::Wink,M::Kiss,G::Wave,A::Hearts,pink},
  {"心动星星",1,E::Star,M::Small,G::Heart,A::Sparkle,purple},
  {"偷偷喜欢",1,E::Side,M::Smile,G::Shy,A::Blush,pink},
  {"求抱抱",1,E::Sad,M::Pout,G::Hug,A::Hearts,purple},
  {"托腮思考",2,E::Look,M::Flat,G::Think,A::Question,gold},
  {"歪眼疑问",2,E::Question,M::Small,G::Shrug,A::Question,cyan},
  {"灵光一闪",2,E::Wide,M::Open,G::Point,A::Sparkle,gold},
  {"认真观察",2,E::Half,M::Flat,G::Think,A::None,cyan},
  {"左右张望",2,E::Side,M::Small,G::Point,A::None,blue},
  {"我有主意",2,E::Wink,M::Grin,G::Thumbs,A::Question,green},
  {"困困打盹",3,E::Sleep,M::Flat,G::Rest,A::Zzz,blue},
  {"大大哈欠",3,E::Closed,M::Yawn,G::Stretch,A::Zzz,white},
  {"眯眼放空",3,E::Half,M::Small,G::Rest,A::None,white},
  {"伸个懒腰",3,E::Squeeze,M::Smile,G::Stretch,A::Sparkle,green},
  {"晚安好梦",3,E::Closed,M::Smile,G::Heart,A::Zzz,purple},
  {"悠闲摇摆",3,E::Sleep,M::Smile,G::Dance,A::None,blue},
  {"委屈巴巴",4,E::Sad,M::Wobble,G::Shy,A::Blush,blue},
  {"眼泪汪汪",4,E::Tear,M::Frown,G::Wipe,A::Tears,cyan},
  {"哇哇大哭",4,E::Squeeze,M::Open,G::Wipe,A::Tears,blue},
  {"失落叹气",4,E::Sad,M::Flat,G::Rest,A::Sweat,purple},
  {"需要安慰",4,E::Tear,M::Pout,G::Hug,A::Hearts,blue},
  {"抱抱自己",4,E::Closed,M::Frown,G::CrossArms,A::Tears,purple},
  {"恶魔斜眼",5,E::Angry,M::Grin,G::CrossArms,A::None,orange},
  {"气鼓鼓",5,E::Angry,M::Pout,G::Shake,A::Anger,pink},
  {"不服气",5,E::Side,M::Frown,G::CrossArms,A::Anger,gold},
  {"哼才没有",5,E::Half,M::Pout,G::Point,A::Blush,purple},
  {"认真加油",5,E::Angry,M::Smile,G::Shake,A::Sparkle,green},
  {"炸毛啦",5,E::Wide,M::Open,G::Cheer,A::Anger,orange},
  {"突然惊讶",6,E::Wide,M::Open,G::Surprise,A::None,gold},
  {"紧张冒汗",6,E::Uneven,M::Wobble,G::Shy,A::Sweat,cyan},
  {"有点害怕",6,E::Uneven,M::Open,G::Surprise,A::Sweat,purple},
  {"一脸震惊",6,E::Cross,M::Open,G::Cheer,A::Question,pink},
  {"尴尬微笑",6,E::Half,M::Grin,G::Shrug,A::Sweat,blue},
  {"手足无措",6,E::Question,M::Wobble,G::Wave,A::Sweat,gold},
  {"吐舌鬼脸",7,E::Wink,M::Tongue,G::Peace,A::None,pink},
  {"晕乎乎",7,E::Spiral,M::Wobble,G::Shrug,A::Question,purple},
  {"发财啦",7,E::Money,M::Grin,G::Dance,A::Sparkle,gold},
  {"胜利欢呼",7,E::Star,M::Laugh,G::Cheer,A::Confetti,green},
  {"扭扭跳舞",7,E::Uneven,M::Tongue,G::Dance,A::Confetti,cyan},
  {"满分奖励",7,E::Happy,M::Grin,G::Thumbs,A::Confetti,orange},
};
static_assert(sizeof(CATALOG)/sizeof(CATALOG[0])==COUNT,"Expression catalog size changed");

bool parseId(const String &value, uint8_t &id) {
  if (value.isEmpty() || value.length() > 2) return false;
  unsigned n = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    n = n * 10 + value[i] - '0';
  }
  if (n >= COUNT) return false;
  id = static_cast<uint8_t>(n);
  return true;
}

bool parseSelection(const String &csv, uint64_t &mask) {
  if (csv.isEmpty() || csv.length() > COUNT * 3 - 1) return false;
  uint64_t selected = 0;
  size_t start = 0;
  for (size_t i = 0; i <= csv.length(); ++i) {
    if (i < csv.length() && csv[i] != ',') continue;
    uint8_t id;
    if (!parseId(csv.substring(start, i), id)) return false;
    const uint64_t bit = uint64_t{1} << id;
    if (selected & bit) return false;
    selected |= bit;
    start = i + 1;
  }
  mask = selected;
  return validMask(mask);
}

uint8_t choose(uint64_t mask, uint8_t previous, uint32_t entropy) {
  mask &= ALL;
  if (!mask) mask=1;
  if ((mask & (mask-1)) && previous<COUNT) mask &= ~(uint64_t{1}<<previous);
  uint8_t count=0;
  for(uint8_t i=0;i<COUNT;++i) if(mask & (uint64_t{1}<<i)) ++count;
  uint8_t rank=entropy%count;
  for(uint8_t i=0;i<COUNT;++i) if(mask & (uint64_t{1}<<i)) {
    if(rank==0) return i;
    --rank;
  }
  return 0;
}
}  // namespace RobotExpressions
