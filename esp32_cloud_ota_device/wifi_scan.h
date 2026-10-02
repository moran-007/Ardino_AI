#pragma once
#include <Arduino.h>

namespace WifiScan {
constexpr size_t MAX_RESULTS = 20;
struct Network { char ssid[33]; int32_t rssi; bool open; };
// Connection attempts require a fresh scan; the web list may reuse its cache.
bool start(bool fresh = false);
void poll();
void cancel();
bool running();
bool failed();
size_t count();
const Network &get(size_t index);
// Only saved networks observed by the last successful scan, strongest first.
// Saved matches are retained separately from the limited web result list.
size_t connectionOrder(uint8_t *order, size_t capacity, const char *preferred = nullptr);
}  // namespace WifiScan
