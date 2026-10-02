#include "wifi_scan.h"

#include <WiFi.h>
#include <esp_wifi.h>
#include <string.h>

#include "wifi_profiles.h"

namespace WifiScan {
namespace {
Network networks[MAX_RESULTS] = {};
Network savedMatches[WifiProfiles::MAX_NETWORKS] = {};
size_t savedMatchCount = 0;
size_t resultCount = 0;
bool scanning = false;
bool scanFailed = false;
bool everScanned = false;
uint32_t lastScanAtMs = 0;

void collect(int found) {
  resultCount = 0;
  savedMatchCount = 0;
  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.isEmpty() || ssid.length() > 32) continue;
    Network next{};
    ssid.toCharArray(next.ssid, sizeof(next.ssid));
    next.rssi = WiFi.RSSI(i);
    next.open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
    if (WifiProfiles::find(ssid)) {
      size_t saved = 0;
      while (saved < savedMatchCount && strcmp(savedMatches[saved].ssid, next.ssid) != 0) ++saved;
      if (saved == savedMatchCount && savedMatchCount < WifiProfiles::MAX_NETWORKS) {
        savedMatches[savedMatchCount++] = next;
      } else if (saved < savedMatchCount && next.rssi > savedMatches[saved].rssi) {
        savedMatches[saved] = next;
      }
    }
    size_t slot = resultCount;
    for (size_t j = 0; j < resultCount; ++j) {
      if (strcmp(networks[j].ssid, next.ssid) == 0) { slot = j; break; }
    }
    if (slot < resultCount && networks[slot].rssi >= next.rssi) continue;
    if (slot == MAX_RESULTS) {
      if (next.rssi <= networks[MAX_RESULTS - 1].rssi) continue;
      slot = MAX_RESULTS - 1;
    } else if (slot == resultCount) ++resultCount;
    networks[slot] = next;
    // At most 20 elements; keep strongest networks first without heap allocation.
    while (slot > 0 && networks[slot].rssi > networks[slot - 1].rssi) {
      const Network previous = networks[slot - 1];
      networks[slot - 1] = networks[slot];
      networks[slot--] = previous;
    }
  }
  WiFi.scanDelete();
  scanning = false;
  scanFailed = found < 0;
}
}  // namespace

bool start(bool fresh) {
  poll();
  if (scanning || (!fresh && everScanned && millis() - lastScanAtMs < 10000)) return !scanFailed;
  lastScanAtMs = millis();
  everScanned = true;
  scanFailed = false;
  resultCount = 0;
  savedMatchCount = 0;
  WiFi.setScanTimeout(10000);
  const int result = WiFi.scanNetworks(true, false, false, 150);
  if (result == WIFI_SCAN_RUNNING) scanning = true;
  else collect(result);
  return !scanFailed;
}

void poll() {
  if (!scanning) return;
  const int result = WiFi.scanComplete();
  if (result != WIFI_SCAN_RUNNING) {
    if (result < 0) esp_wifi_scan_stop();
    collect(result);
  }
}
void cancel() {
  if (scanning) esp_wifi_scan_stop();
  collect(WIFI_SCAN_FAILED);
}
bool running() { return scanning; }
bool failed() { return scanFailed; }
size_t count() { return resultCount; }
const Network &get(size_t index) { return networks[index]; }

size_t connectionOrder(uint8_t *order, size_t capacity, const char *preferred) {
  if (!order || capacity == 0 || scanning || scanFailed || !everScanned) return 0;
  capacity = min(capacity, WifiProfiles::MAX_NETWORKS);
  int32_t strengths[WifiProfiles::MAX_NETWORKS] = {};
  size_t added = 0;
  for (size_t i = 0; i < WifiProfiles::count(); ++i) {
    for (size_t n = 0; n < savedMatchCount; ++n) {
      if (strcmp(WifiProfiles::get(i)->ssid, savedMatches[n].ssid) != 0) continue;
      if (added == capacity && savedMatches[n].rssi <= strengths[added - 1]) break;
      size_t slot = added < capacity ? added++ : added - 1;
      while (slot > 0 && savedMatches[n].rssi > strengths[slot - 1]) {
        order[slot] = order[slot - 1];
        strengths[slot] = strengths[slot - 1];
        --slot;
      }
      order[slot] = static_cast<uint8_t>(i);
      strengths[slot] = savedMatches[n].rssi;
      break;
    }
  }
  // A manually chosen network gets priority only if it is actually visible.
  for (size_t i = 0; preferred && i < added; ++i) {
    if (strcmp(preferred, WifiProfiles::get(order[i])->ssid) != 0) continue;
    const uint8_t selected = order[i];
    for (size_t j = i; j > 0; --j) order[j] = order[j - 1];
    order[0] = selected;
    break;
  }
  return added;
}
}  // namespace WifiScan
