#pragma once

#include <Arduino.h>
#include <FS.h>

// Experimental, speaker-dependent acoustic enrollment. The label is descriptive:
// changing text never retrains Espressif WakeNet. No listening audio leaves FS/RAM.
namespace CustomWake {
constexpr size_t FRAME_SAMPLES = 320;
constexpr uint8_t MAX_SAMPLES = 3;

bool begin(fs::FS &storage);
String toJson();
bool setLabel(const String &label);
bool enable(bool enabled);
bool reset();
bool queueTraining();
void cancelTraining();
bool trainingPending();
bool trainingActive();
// Main-loop foreground only, after conversation/playback/alarm/OTA is idle.
// service may pump the authenticated web server for status/cancel requests.
bool processQueuedTraining(void (*service)() = nullptr);
bool recordSample(void (*service)() = nullptr);
bool enabled();
bool ready();
uint8_t sampleCount();
const char *label();
const char *lastError();
// Listener-task only; mutating calls suspend it before touching templates.
bool process(const int16_t *samples, size_t count);
void resetDetection();
}  // namespace CustomWake
