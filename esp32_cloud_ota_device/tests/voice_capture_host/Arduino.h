#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

#define ARDUINO 10800
#define CONFIG_IDF_TARGET_ESP32S3 1
#define INPUT 0
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define ADC_11db 3
#define PI 3.14159265358979323846
#define F(value) value

using std::min;
using std::max;

class String {
 public:
  String() = default;
  String(const char *value) : value_(value ? value : "") {}
  String(const std::string &value) : value_(value) {}
  template <class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T value) : value_(std::to_string(value)) {}
  friend String operator+(const String &a, const String &b) { return a.value_ + b.value_; }
  const char *c_str() const { return value_.c_str(); }

 private:
  std::string value_;
};

template <class T, class A, class B> T constrain(T value, A low, B high) {
  return static_cast<T>(value < static_cast<T>(low) ? low : value > static_cast<T>(high) ? high : value);
}

class Stream {
 public:
  virtual ~Stream() = default;
  virtual size_t readBytes(char *buffer, size_t length) = 0;
};

inline uint32_t fakeMillis = 1000;
inline uint32_t millis() { return fakeMillis; }
inline uint32_t micros() { return fakeMillis * 1000U; }
inline void delay(uint32_t ms) { fakeMillis += ms; }
inline void yield() {}
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline uint16_t analogRead(int) { return 0; }
inline void analogReadResolution(int) {}
inline void analogSetPinAttenuation(int, int) {}

struct FakeSerial {
  template <class... T> void printf(const char *, T...) {}
  void println(const char *) {}
};
inline FakeSerial Serial;
