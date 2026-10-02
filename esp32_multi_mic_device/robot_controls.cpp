#include "robot_controls.h"

namespace RobotControls {
namespace {

volatile bool abortLatched = false;
bool initialized = false;

void ARDUINO_ISR_ATTR onAbortPressed() {
  abortLatched = true;
}

}  // namespace

void begin() {
  if (initialized) return;
  pinMode(PIN_TALK_BUTTON, INPUT_PULLUP);
  pinMode(PIN_ABORT_BUTTON, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ABORT_BUTTON), onAbortPressed, FALLING);
  initialized = true;
}

bool talkPressed() {
  return digitalRead(PIN_TALK_BUTTON) == LOW;
}

bool abortRequested() {
  return abortLatched || digitalRead(PIN_ABORT_BUTTON) == LOW;
}

bool consumeAbortRequest() {
  noInterrupts();
  const bool requested = abortLatched;
  abortLatched = false;
  interrupts();
  return requested || digitalRead(PIN_ABORT_BUTTON) == LOW;
}

void clearAbortRequest() {
  noInterrupts();
  abortLatched = false;
  interrupts();
}

void requestAbort() {
  noInterrupts();
  abortLatched = true;
  interrupts();
}

}  // namespace RobotControls
