#pragma once
#include <Arduino.h>

class Stream {
 public:
  virtual ~Stream() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *, size_t) = 0;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int peek() = 0;
  void setWriteError(int value = 1) { error_ = value; }
  int getWriteError() const { return error_; }
  void clearWriteError() { error_ = 0; }

 private:
  int error_ = 0;
};
