#pragma once

#include <Arduino.h>

namespace WifiProfiles {
constexpr size_t MAX_NETWORKS = 5;
struct Profile { char ssid[33]; char password[64]; };
bool begin(const String &legacySsid, const String &legacyPassword);
size_t count();
const Profile *get(size_t index);
const Profile *find(const String &ssid);
bool save(const String &ssid, const String &password);
bool remove(const String &ssid);
}  // namespace WifiProfiles
