#pragma once

#include <Arduino.h>

namespace OfflineWake {

struct ModelOption {
  const char *id;
  const char *label;
};

constexpr char DEFAULT_MODEL[] = "wn9_nihaoxiaozhi_tts";

bool begin(bool enabled, const String &modelId);
bool configure(bool enabled, const String &modelId);
void suspend();
// True only after the listener acknowledges suspension and releases the mic.
bool paused();
void resume();
bool consumeDetected();
bool ready();
bool enabled();
bool listening();
// Listener may be between read/detect calls while still enabled and running.
bool listenerActive();
const char *modelId();
const char *wakeWord();
const char *lastError();
size_t modelOptionCount();
const ModelOption &modelOption(size_t index);
bool isSupportedModel(const String &modelId);

}  // namespace OfflineWake
