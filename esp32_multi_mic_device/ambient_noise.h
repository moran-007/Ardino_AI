#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Gain-independent lower-quartile noise estimate. Speech bursts do not drag the
// background level upward; a sustained environmental change settles gradually.
class AmbientNoise {
 public:
  void reset() { count_ = cursor_ = 0; floor_ = 0; initialized_ = false; }
  void observe(float rms) {
    if (!std::isfinite(rms) || rms < 0) return;
    values_[cursor_++ % 64] = rms;
    if (count_ < 64) ++count_;
    if (count_ < 64 || cursor_ % 16 != 0) return;
    float ordered[64];
    std::copy(values_, values_ + 64, ordered);
    std::sort(ordered, ordered + 64);
    const float candidate = ordered[16];
    floor_ = initialized_ ? floor_ + (candidate - floor_) * .15f : candidate;
    initialized_ = true;
  }
  bool ready() const { return count_ == 64; }
  float floor() const { return ready() ? floor_ : 0; }
  uint32_t threshold(uint32_t fixed, uint8_t gain) const {
    return std::max(fixed, static_cast<uint32_t>(floor() * gain * 2.5f + 20));
  }
 private:
  float values_[64] = {};
  uint32_t count_ = 0, cursor_ = 0;
  float floor_ = 0;
  bool initialized_ = false;
};
