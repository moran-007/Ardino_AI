#pragma once
#include <Arduino.h>
namespace FakeNetwork { inline unsigned active = 0, stops = 0; }
class WiFiClientSecure {
 public:
  void setCACert(const char *) {}
  void setHandshakeTimeout(unsigned) {}
  void stop() { if (opened) { --FakeNetwork::active; opened = false; } ++FakeNetwork::stops; }
  bool opened = false;
};
