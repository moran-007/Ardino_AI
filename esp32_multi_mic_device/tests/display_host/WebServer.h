#pragma once
#include "Arduino.h"
#define CONTENT_LENGTH_UNKNOWN -1
class WebServer {
 public:
  std::string html;
  int status=0;
  void sendHeader(const char*,const char*){}
  void setContentLength(int){}
  void send(int code,const char*,const char*){status=code;}
  void sendContent_P(const char *s){html+=s;}
  void sendContent(const String &s){html+=s.c_str();}
  void sendContent(const char *s,size_t n){html.append(s,n);}
};
