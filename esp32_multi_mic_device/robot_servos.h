#pragma once

#include "hardware_pins.h"

namespace RobotServos {

constexpr size_t COUNT = 4;
constexpr int PINS[COUNT] = {
    HardwarePins::SERVO_1, HardwarePins::SERVO_2,
    HardwarePins::SERVO_3, HardwarePins::SERVO_4,
};
constexpr uint8_t DEFAULT_ANGLES[COUNT] = {0, 0, 180, 180};
constexpr uint32_t PWM_HZ = 50;
constexpr uint8_t PWM_BITS = 14;
// Nominal SG90 180-degree pulse endpoints. Calibrate to the actual servos.
constexpr uint16_t MIN_PULSE_US = 500;
constexpr uint16_t MAX_PULSE_US = 2500;

// begin() immediately commands DEFAULT_ANGLES. No sweep or interpolation.
bool begin();
bool ready();
// Last commanded angle and whether PWM is currently active; no position sensor.
uint8_t angle(uint8_t channel);
bool outputActive(uint8_t channel);
// Channel numbers are 1..4; angles are 0..180. Explicit moves enable the output.
bool setAngle(uint8_t channel, uint16_t degrees);
bool setAngles(const uint16_t (&degrees)[COUNT]);
bool home();
// Removes PWM pulses, but does not disconnect external servo power.
void release();
String status();
bool executeCommand(const String &command, String &message);

}  // namespace RobotServos
