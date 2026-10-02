#pragma once

#include <Arduino.h>

#include "hardware_pins.h"

namespace RobotControls {

// External momentary buttons. Both use INPUT_PULLUP and are active LOW.
constexpr int PIN_TALK_BUTTON = HardwarePins::TALK_BUTTON;
constexpr int PIN_ABORT_BUTTON = HardwarePins::ABORT_BUTTON;
constexpr int PIN_BOOT_BUTTON = HardwarePins::BOOT_BUTTON;

void begin();
bool talkPressed();
bool abortRequested();
bool consumeAbortRequest();
void clearAbortRequest();
void requestAbort();

}  // namespace RobotControls
