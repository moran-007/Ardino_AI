#pragma once
#include <Stream.h>

namespace WebAdmin {
// HTTPClient decodes upstream chunks; WebServer sends them without a full-body String.
template <typename Server>
class ResponseStream : public Stream {
 public:
  explicit ResponseStream(Server &server, size_t limit) : server_(server), limit_(limit) {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t *data, size_t size) override {
    if (!size) return 0;
    if (failed_ || size > limit_ - sent_) {
      failed_ = true;
      setWriteError();
      return 0;
    }
    server_.sendContent(reinterpret_cast<const char *>(data), size);
    sent_ += size;
    return size;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  bool failed() const { return failed_; }

 private:
  Server &server_;
  const size_t limit_;
  size_t sent_ = 0;
  bool failed_ = false;
};
}  // namespace WebAdmin
