#include "wifi_profiles.h"

#include <Preferences.h>
#include <string.h>

namespace WifiProfiles {
namespace {
struct StoredProfiles {
  uint8_t version = 1;
  uint8_t count = 0;
  Profile items[MAX_NETWORKS] = {};
};
StoredProfiles stored;

bool persist(const StoredProfiles &next) {
  Preferences storage;
  if (!storage.begin("robot-wifi", false)) return false;
  const bool ok = storage.putBytes("profiles", &next, sizeof(next)) == sizeof(next);
  storage.end();
  if (ok) stored = next;
  return ok;
}
}  // namespace

bool begin(const String &legacySsid, const String &legacyPassword) {
  Preferences storage;
  if (!storage.begin("robot-wifi", false)) return false;
  const bool exists = storage.isKey("profiles");
  StoredProfiles loaded;
  const bool read = storage.getBytesLength("profiles") == sizeof(loaded) &&
      storage.getBytes("profiles", &loaded, sizeof(loaded)) == sizeof(loaded);
  storage.end();
  if (exists) {
    if (!read || loaded.version != 1 || loaded.count > MAX_NETWORKS) return false;
    for (size_t i = 0; i < loaded.count; ++i) {
      if (strnlen(loaded.items[i].ssid, 33) == 0 ||
          strnlen(loaded.items[i].ssid, 33) > 32 ||
          strnlen(loaded.items[i].password, 64) > 63) return false;
    }
    stored = loaded;
    return true;
  }
  return legacySsid.isEmpty() ? persist(stored) : save(legacySsid, legacyPassword);
}

size_t count() { return stored.count; }
const Profile *get(size_t index) { return index < stored.count ? &stored.items[index] : nullptr; }
const Profile *find(const String &ssid) {
  for (size_t i = 0; i < stored.count; ++i)
    if (ssid == stored.items[i].ssid) return &stored.items[i];
  return nullptr;
}

bool save(const String &ssid, const String &password) {
  if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
      (!password.isEmpty() && password.length() < 8)) return false;
  const Profile *existing = find(ssid);
  if (existing && password == existing->password) return true;
  StoredProfiles next;
  ssid.toCharArray(next.items[0].ssid, sizeof(next.items[0].ssid));
  password.toCharArray(next.items[0].password, sizeof(next.items[0].password));
  next.count = 1;
  for (size_t i = 0; i < stored.count && next.count < MAX_NETWORKS; ++i) {
    if (ssid != stored.items[i].ssid) next.items[next.count++] = stored.items[i];
  }
  return persist(next);
}

bool remove(const String &ssid) {
  if (!find(ssid)) return false;
  StoredProfiles next;
  for (size_t i = 0; i < stored.count; ++i)
    if (ssid != stored.items[i].ssid) next.items[next.count++] = stored.items[i];
  return persist(next);
}
}  // namespace WifiProfiles
