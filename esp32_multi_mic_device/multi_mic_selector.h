#pragma once

#include <stddef.h>
#include <stdint.h>
#include <math.h>

namespace MultiMic {

constexpr uint8_t DEFAULT_INSTALLED_MASK = 1;
constexpr uint32_t SWITCH_HOLD_SAMPLES = 4000;  // 250 ms at 16 kHz.
struct Stereo32 { int32_t left; int32_t right; };

inline bool validConfiguration(uint8_t mask, uint8_t preferred) {
  return mask >= 1 && mask <= 7 && preferred <= 3 &&
         (preferred == 0 || (mask & (1U << (preferred - 1))));
}

// Select one complete channel, never sum independently clocked microphones.
// RMS removes the block's DC offset; values are measured before capture gain.
class Selector {
 public:
  bool configure(uint8_t mask, uint8_t preferred) {
    if (!validConfiguration(mask, preferred)) return false;
    mask_ = mask;
    preferred_ = preferred;
    reset();
    return true;
  }
  void reset(bool preserveSelection = false) {
    if (preferred_) selected_ = preferred_;
    else if (!preserveSelection || !(mask_ & (1U << (selected_ - 1)))) selected_ = firstInstalled();
    candidate_ = 0;
    candidateSamples_ = 0;
    blocks_ = 0;
    for (uint8_t c = 0; c < 3; ++c) { rms_[c] = peak_[c] = 0; }
  }
  uint8_t update(const Stereo32 *primary, const Stereo32 *secondary, size_t frames, bool allowSwitch = true, uint32_t sampleRate = 16000) {
    uint8_t best = selected_;
    for (uint8_t c = 0; c < 3; ++c) {
      rms_[c] = peak_[c] = 0;
      if (!(mask_ & (1U << c)) || !(c == 2 ? secondary : primary) || frames == 0) continue;
      int64_t sum = 0;
      uint64_t squares = 0;
      for (size_t i = 0; i < frames; ++i) {
        const int32_t sample = (c == 2 ? secondary[i].left :
                                c == 0 ? primary[i].left : primary[i].right) >> 16;
        sum += sample;
        squares += static_cast<int64_t>(sample) * sample;
        const uint32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak_[c]) peak_[c] = magnitude;
      }
      const double mean = static_cast<double>(sum) / frames;
      const double variance = static_cast<double>(squares) / frames - mean * mean;
      rms_[c] = static_cast<uint16_t>(sqrt(variance > 0 ? variance : 0));
    }
    ++blocks_;
    if (preferred_) return selected_ = preferred_;
    if (!allowSwitch) { candidate_ = 0; candidateSamples_ = 0; return selected_; }
    for (uint8_t c = 0; c < 3; ++c) {
      if ((mask_ & (1U << c)) && rms_[c] > rms_[best - 1]) best = c + 1;
    }
    // Require sustained improvement; silence and brief taps cannot switch.
    if (best != selected_ && rms_[best - 1] >= 32 &&
        static_cast<uint32_t>(rms_[best - 1]) * 10U >
            static_cast<uint32_t>(rms_[selected_ - 1]) * 18U) {
      if (candidate_ != best) { candidate_ = best; candidateSamples_ = 0; }
      candidateSamples_ += static_cast<uint32_t>(frames);
      if (candidateSamples_ >= sampleRate / 4) {
        selected_ = best;
        candidate_ = 0;
        candidateSamples_ = 0;
      }
    } else { candidate_ = 0; candidateSamples_ = 0; }
    return selected_;
  }
  uint8_t mask() const { return mask_; }
  uint8_t preferred() const { return preferred_; }
  uint8_t selected() const { return selected_; }
  uint16_t rms(uint8_t c) const { return c < 3 ? rms_[c] : 0; }
  uint16_t peak(uint8_t c) const { return c < 3 ? peak_[c] : 0; }
  uint32_t blocks() const { return blocks_; }

 private:
  uint8_t firstInstalled() const {
    for (uint8_t c = 0; c < 3; ++c) if (mask_ & (1U << c)) return c + 1;
    return 1;
  }
  uint8_t mask_ = DEFAULT_INSTALLED_MASK;
  uint8_t preferred_ = 0;
  uint8_t selected_ = 1;
  uint8_t candidate_ = 0;
  uint32_t candidateSamples_ = 0;
  uint32_t blocks_ = 0;
  uint16_t rms_[3] = {};
  uint16_t peak_[3] = {};
};

}  // namespace MultiMic
