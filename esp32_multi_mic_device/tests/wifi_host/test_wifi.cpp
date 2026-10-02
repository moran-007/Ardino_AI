#include <cassert>
#include <iostream>
#include "WiFi.h"
#include "esp_wifi.h"
#include "wifi_scan.h"
#include "wifi_profiles.h"

// Hardware and NVS are substituted; candidate selection and scan lifecycle
// below execute the production wifi_scan.cpp.
namespace WifiProfiles {
std::vector<Profile> testProfiles;
size_t count() { return testProfiles.size(); }
const Profile *get(size_t i) { return i < count() ? &testProfiles[i] : nullptr; }
const Profile *find(const String &ssid) {
  for (const auto &p : testProfiles) if (ssid == p.ssid) return &p;
  return nullptr;
}
}

void completeScan(std::vector<FakeAccessPoint> aps) {
  WiFi.aps = aps;
  WiFi.completion = WIFI_SCAN_RUNNING;
  WiFi.startResult = WIFI_SCAN_RUNNING;
  assert(WifiScan::start(true));
  assert(WifiScan::running());
  WiFi.completion = static_cast<int>(aps.size());
  WifiScan::poll();
  assert(!WifiScan::running() && !WifiScan::failed());
}

int main() {
  WifiProfiles::testProfiles = {{"Home", "password"}, {"Office", "password"}, {"Gone", "password"}};
  uint8_t order[5] = {};
  assert(WifiScan::connectionOrder(order, 5) == 0);
  completeScan({{"Stranger", -20}, {"Home", -75}, {"Office", -35}, {"Home", -60}});
  assert(WifiScan::count() == 3);
  assert(WiFi.timeout == 10000);
  assert(WifiScan::connectionOrder(order, 5) == 2 && order[0] == 1 && order[1] == 0);
  // A nonexistent saved or manually preferred network must never be attempted.
  assert(WifiScan::connectionOrder(order, 5, "Gone") == 2 && order[0] == 1);
  assert(WifiScan::connectionOrder(order, 5, "Home") == 2 && order[0] == 0);
  assert(WifiScan::connectionOrder(order, 1) == 1 && order[0] == 1);
  assert(WifiScan::connectionOrder(order, 0) == 0);

  const unsigned starts = WiFi.starts;
  assert(WifiScan::start() && WiFi.starts == starts); // UI cache
  WiFi.completion = WIFI_SCAN_RUNNING;
  assert(WifiScan::start(true) && WiFi.starts == starts + 1); // reconnect is fresh
  assert(WifiScan::count() == 0 && WifiScan::connectionOrder(order, 5) == 0);
  const unsigned stops = stoppedScans;
  WifiScan::cancel();
  assert(WifiScan::failed() && !WifiScan::running() && stoppedScans == stops + 1);
  assert(WifiScan::connectionOrder(order, 5) == 0);

  // A known AP below the UI's strongest 20 entries must remain connectable.
  std::vector<FakeAccessPoint> crowded;
  for (int i = 0; i < 28; ++i) crowded.push_back({String("Neighbor") + String(i), -20 - i});
  crowded.push_back({"Home", -89});
  completeScan(crowded);
  assert(WifiScan::count() == 20);
  assert(WifiScan::connectionOrder(order, 5) == 1 && order[0] == 0);

  // Empty and failed scans clear previously visible saved networks.
  completeScan({});
  assert(WifiScan::connectionOrder(order, 5) == 0);
  WiFi.startResult = WIFI_SCAN_FAILED;
  assert(!WifiScan::start(true));
  assert(WifiScan::failed() && WifiScan::connectionOrder(order, 5) == 0);
  completeScan({{"Office", -40}});
  WifiProfiles::testProfiles.erase(WifiProfiles::testProfiles.begin() + 1);
  assert(WifiScan::connectionOrder(order, 5) == 0); // forgotten during cache lifetime
  std::cout << "Wi-Fi scan/candidate regression checks passed.\n";
}
