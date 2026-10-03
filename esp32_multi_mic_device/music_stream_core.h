#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MusicStream {
// Callers serialize access; storage is PSRAM on the device, ordinary RAM in tests.
class Ring {
 public:
  void attach(uint8_t *storage, size_t capacity) { data_ = storage; capacity_ = capacity; clear(); }
  void clear() { head_ = tail_ = used_ = 0; }
  size_t size() const { return used_; }
  size_t space() const { return capacity_ - used_; }
  size_t write(const uint8_t *data, size_t count) {
    if (!data_ || !data || !count) return 0;
    if (count > space()) count = space();
    const size_t first = count < capacity_ - tail_ ? count : capacity_ - tail_;
    memcpy(data_ + tail_, data, first); memcpy(data_, data + first, count - first);
    tail_ = (tail_ + count) % capacity_; used_ += count; return count;
  }
  size_t peek(uint8_t *data, size_t count) const {
    if (!data_ || !data || !count) return 0;
    if (count > used_) count = used_;
    const size_t first = count < capacity_ - head_ ? count : capacity_ - head_;
    memcpy(data, data_ + head_, first); memcpy(data + first, data_, count - first); return count;
  }
  void consume(size_t count) { if (!capacity_) return; if (count > used_) count = used_; head_ = (head_ + count) % capacity_; used_ -= count; }
 private:
  uint8_t *data_ = nullptr; size_t capacity_ = 0, head_ = 0, tail_ = 0, used_ = 0;
};

struct Intent {
  uint32_t generation = 1;
  uint32_t holds = 0;
  bool playing = false;
  uint64_t renderedFrames = 0, downloadedBytes = 0;
  void newTrack() { ++generation; playing = true; renderedFrames = downloadedBytes = 0; }
  void stop() { ++generation; playing = false; renderedFrames = downloadedBytes = 0; }
  void pause() { playing = false; }
  void resume() { playing = true; }
  void hold() { ++holds; }
  void release() { if (holds) --holds; }
  bool downloadAllowed() const { return playing && !holds; }
  bool accepts(uint32_t ticket) const { return ticket == generation; }
  uint64_t positionMs() const { return renderedFrames * 1000ULL / 48000ULL; }
};
}
