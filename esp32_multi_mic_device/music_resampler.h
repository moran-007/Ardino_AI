#pragma once
#include <math.h>
#include <stdint.h>

namespace MusicStream {
// 79-tap Hamming low-pass (6.4 kHz cutoff), before 48 -> 16 kHz decimation.
// Persistent phase/history prevent seams between WakeNet read blocks.
class Decimator {
 public:
  Decimator() {
    double sum = 0;
    for (int i = 0; i < TAPS; ++i) {
      const int x = i - (TAPS - 1) / 2;
      const double f = 6400.0 / 48000.0;
      coefficients_[i] = static_cast<float>((x ? sin(2 * PI_VALUE * f * x) / (PI_VALUE * x) : 2 * f) *
          (0.54 - 0.46 * cos(2 * PI_VALUE * i / (TAPS - 1))));
      sum += coefficients_[i];
    }
    for (auto &v : coefficients_) v /= static_cast<float>(sum);
    reset();
  }
  void reset() { for (auto &v : history_) v = 0; index_ = phase_ = 0; }
  bool push(float sample, float &result) {
    history_[index_] = sample; index_ = (index_ + 1) % TAPS;
    if (++phase_ < 3) return false;
    phase_ = 0; result = 0;
    int p = index_;
    for (int i = 0; i < TAPS; ++i) { if (--p < 0) p = TAPS - 1; result += history_[p] * coefficients_[i]; }
    return true;
  }
 private:
  static constexpr int TAPS = 79;
  static constexpr double PI_VALUE = 3.14159265358979323846;
  float history_[TAPS], coefficients_[TAPS]; int index_, phase_;
};
}
