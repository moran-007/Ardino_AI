#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <string>
#include <type_traits>
#include <ctime>
#include <cmath>
#include <functional>
class Stream;
inline float radians(float degrees){return degrees*3.14159265358979323846f/180.0f;}
#define ARDUINO 10800
#define CONFIG_IDF_TARGET_ESP32S3 1
#define PROGMEM
#define INPUT 0
#define OUTPUT 1
#define HIGH 1
#define LOW 0
#define ADC_11db 3
#define ARDUINO_RUNNING_CORE 1
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
using std::min;
using std::max;
using PGM_P = const char *;
class __FlashStringHelper;
#define F(s) reinterpret_cast<const __FlashStringHelper *>(s)
class String {
 public:
  String() = default;
  String(const char *s): value(s ? s : "") {}
  String(const __FlashStringHelper *s): value(reinterpret_cast<const char *>(s)) {}
  String(const std::string &s): value(s) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T n): value(std::to_string(n)) {}
  bool isEmpty() const { return value.empty(); }
  size_t length() const { return value.size(); }
  const char *c_str() const { return value.c_str(); }
  char operator[](size_t i) const { return value[i]; }
  void reserve(size_t n) { value.reserve(n); }
  void toCharArray(char *out, size_t n) const { std::strncpy(out,value.c_str(),n-1); out[n-1]=0; }
  void trim() { auto a=value.find_first_not_of(" \r\n\t"), b=value.find_last_not_of(" \r\n\t"); value=a==std::string::npos?"":value.substr(a,b-a+1); }
  void toLowerCase() { for(char &c:value)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
  int indexOf(char c) const { auto i=value.find(c); return i==std::string::npos?-1:static_cast<int>(i); }
  int indexOf(const char *c) const { auto i=value.find(c); return i==std::string::npos?-1:static_cast<int>(i); }
  int indexOf(const char *c, size_t start) const { auto i=value.find(c,start); return i==std::string::npos?-1:static_cast<int>(i); }
  int indexOf(const String &s) const { return indexOf(s.c_str()); }
  int indexOf(const String &s, size_t start) const { return indexOf(s.c_str(),start); }
  bool startsWith(const char *s) const { return value.rfind(s,0)==0; }
  bool startsWith(const String &s) const { return startsWith(s.c_str()); }
  bool endsWith(const char *s) const { const size_t n=std::strlen(s);return value.size()>=n&&value.compare(value.size()-n,n,s)==0; }
  void remove(size_t start, size_t n=std::string::npos) { if(start<value.size())value.erase(start,n); }
  long toInt() const { return std::strtol(value.c_str(),nullptr,10); }
  bool concat(const char *s) { if(s)value+=s;return true; }
  bool concat(const char *s, size_t n) { if(s)value.append(s,n);return true; }
  String substring(size_t a) const { return a>value.size()?String():String(value.substr(a)); }
  String substring(size_t a, size_t b) const { return value.substr(a,b-a); }
  void replace(const char *from,const char *to) { size_t i=0; while((i=value.find(from,i))!=std::string::npos){value.replace(i,std::strlen(from),to);i+=std::strlen(to);} }
  String &operator+=(const String &s) { value+=s.value; return *this; }
  String &operator+=(char c) { value+=c; return *this; }
  bool operator==(const String &s) const { return value==s.value; }
  bool operator!=(const String &s) const { return value!=s.value; }
  friend String operator+(const String &a,const String &b) { return a.value+b.value; }
 private:
  std::string value;
};
template<class T,class A,class B> T constrain(T v,A lo,B hi) { return static_cast<T>(v<lo?lo:v>hi?hi:v); }
inline size_t strlcpy(char *out,const char *in,size_t n) { if(n){std::strncpy(out,in,n-1);out[n-1]=0;}return std::strlen(in); }
inline uint32_t fakeMillis=10000;
inline uint16_t fakeAdc=0;
inline bool fakePwmFailure=false;
inline uint32_t fakeDuty=0;
inline uint32_t millis(){return fakeMillis;}
inline std::function<void()> fakeDelayHook;
inline void delay(unsigned n){fakeMillis+=n;if(fakeDelayHook)fakeDelayHook();}
inline void yield(){}
inline void pinMode(int,int){}
inline void digitalWrite(int,int){}
inline void analogSetPinAttenuation(int,int){}
inline uint16_t analogRead(int){return fakeAdc;}
inline uint32_t analogReadMilliVolts(int){return 0;}
inline bool ledcAttach(uint8_t,uint32_t,uint8_t){return !fakePwmFailure;}
inline bool ledcWrite(uint8_t,uint32_t duty){fakeDuty=duty;return !fakePwmFailure;}
inline bool ledcDetach(uint8_t){return true;}
inline bool getLocalTime(tm *out,unsigned){*out={};out->tm_year=126;out->tm_mon=8;out->tm_mday=29;out->tm_hour=10;out->tm_min=28;out->tm_wday=2;return true;}
struct FakeSerial { template<class...T>void printf(const char*,T...){} template<class T>void println(T){} };
inline FakeSerial Serial;
struct FakeEsp {unsigned getFreeHeap(){return 200000;}unsigned getMaxAllocHeap(){return 100000;} };
inline FakeEsp ESP;
