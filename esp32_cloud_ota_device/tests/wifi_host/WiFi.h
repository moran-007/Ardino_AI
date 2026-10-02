#pragma once
#include "Arduino.h"
#include <vector>

constexpr int WIFI_SCAN_RUNNING = -1;
constexpr int WIFI_SCAN_FAILED = -2;
constexpr int WIFI_AUTH_OPEN = 0;
struct FakeAccessPoint { String ssid; int32_t rssi; int auth = 1; };
struct FakeWifi {
  std::vector<FakeAccessPoint> aps;
  int startResult = WIFI_SCAN_RUNNING;
  int completion = WIFI_SCAN_RUNNING;
  unsigned starts = 0, releases = 0;
  uint32_t timeout = 0;
  void setScanTimeout(uint32_t value) { timeout = value; }
  int scanNetworks(bool, bool, bool, int) { ++starts; return startResult; }
  int scanComplete() { return completion; }
  String SSID(int i) { return aps.at(i).ssid; }
  int32_t RSSI(int i) { return aps.at(i).rssi; }
  int encryptionType(int i) { return aps.at(i).auth; }
  void scanDelete() { ++releases; }
};
inline FakeWifi WiFi;
