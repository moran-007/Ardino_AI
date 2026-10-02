#include "Arduino.h"
#include "../../robot_servos.h"
#include <iostream>
int main() {
  if (!RobotServos::begin()) return 2;
  const size_t before = servoWrites.size();
  String message;
  if (!RobotServos::executeCommand("greet", message) && servoWrites.size() == before) {
    std::cout << "RED: existing servo module has no greeting gesture; no PWM movement emitted.\n";
    return 1;
  }
  return 0;
}
