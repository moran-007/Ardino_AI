#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

// The passed filesystem owns the primary copy; use SD here after migration.
namespace PersonalProfile {
constexpr size_t MAX_ITEMS = 24;
constexpr size_t MAX_VALUE_BYTES = 180;
struct CommandResult { bool handled = false; bool changed = false; String message; };
bool begin(fs::FS &storage);
bool ready();
uint32_t revision();
String toJson();
void appendSnapshot(JsonObject object);
bool setPolicy(bool enabled, bool autoCollect, bool backupEnabled);
bool add(const String &category, const String &value, const String &source = "explicit");
bool remove(uint32_t id);
bool edit(uint32_t id, const String &value);
bool clear();
// Restore is explicit; it creates a newer local revision and never auto-merges.
bool restore(JsonVariantConst snapshot);
CommandResult observe(const String &text);
CommandResult handleNaturalLanguage(const String &text);
const char *lastError();
}
