#pragma once
#include "Arduino.h"
#include <map>
inline bool failNvsWrite=false;
inline std::map<std::string,uint64_t> fakeNvs;
inline std::map<std::string,std::string> fakeNvsStrings;
class Preferences {
 public:
  bool begin(const char *s,bool){prefix=std::string(s)+"/";return true;}
  void end(){}
  uint16_t getUShort(const char *s,uint16_t fallback){auto it=fakeNvs.find(prefix+s);return it==fakeNvs.end()?fallback:it->second;}
  uint8_t getUChar(const char *s,uint8_t fallback){return getUShort(s,fallback);}
  uint64_t getULong64(const char *s,uint64_t fallback){auto it=fakeNvs.find(prefix+s);return it==fakeNvs.end()?fallback:it->second;}
  uint32_t getUInt(const char *s,uint32_t fallback){return static_cast<uint32_t>(getULong64(s,fallback));}
  bool getBool(const char *s,bool fallback){return getUShort(s,fallback)!=0;}
  String getString(const char *s,const char *fallback){auto it=fakeNvsStrings.find(prefix+s);return it==fakeNvsStrings.end()?String(fallback):String(it->second);}
  size_t putUShort(const char *s,uint16_t value){if(failNvsWrite)return 0;fakeNvs[prefix+s]=value;return 2;}
  size_t putUChar(const char *s,uint8_t value){return putUShort(s,value)?1:0;}
  size_t putBool(const char *s,bool value){return putUChar(s,value);}
  size_t putULong64(const char *s,uint64_t value){if(failNvsWrite)return 0;fakeNvs[prefix+s]=value;return 8;}
  size_t putString(const char *s,const char *value){if(failNvsWrite)return 0;fakeNvsStrings[prefix+s]=value;return std::strlen(value);}
  size_t putString(const char *s,const String &value){return putString(s,value.c_str());}
  size_t putUInt(const char *s,uint32_t value){return putULong64(s,value)?4:0;}
 private:std::string prefix;
};
