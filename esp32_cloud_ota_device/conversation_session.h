#pragma once

#include <stdint.h>

// One session owns all follow-up turns. Silence, cancellation and errors end it.
class ConversationSession {
 public:
  void start(bool enabled) { active_ = enabled; pending_ = false; }
  void cancel() { active_ = false; pending_ = false; }
  void complete(bool successful, uint32_t now) {
    if (!successful) { cancel(); return; }
    pending_ = active_;
    nextAt_ = now + 600U;
  }
  bool active() const { return active_; }
  bool takeNext(uint32_t now) {
    if (!active_ || !pending_ || static_cast<int32_t>(now - nextAt_) < 0)
      return false;
    pending_ = false;
    return true;
  }

 private:
  bool active_ = false;
  bool pending_ = false;
  uint32_t nextAt_ = 0;
};
