#include "Arduino.h"
#include "Preferences.h"
#include <ArduinoJson.h>
#include "../../robot_interaction.h"
#include "../../robot_servos.h"
#include "../../web_interaction_ui.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
size_t checks = 0;
void require(bool value, const char *message) { ++checks;if (!value) throw std::runtime_error(message); }
JsonDocument state() {
  JsonDocument result;require(!deserializeJson(result, RobotInteraction::toJson().c_str()), "status must be valid JSON");return result;
}
RobotInteraction::Config config(bool enabled = true, uint8_t channel = 1, uint8_t low = 0, uint8_t high = 20) {
  RobotInteraction::Config value;value.enabled = enabled;value.channel = channel;value.lowDegrees = low;value.highDegrees = high;return value;
}
void advance(uint32_t duration = 40) { fakeMillis += duration;RobotInteraction::poll(); }
void complete(uint8_t channel, uint8_t original) {
  uint8_t previous = RobotServos::angle(channel);size_t ticks = 0;
  while (RobotInteraction::busy() && ticks++ < 100) {
    advance();const uint8_t next = RobotServos::angle(channel);
    require(abs(int(next) - int(previous)) <= 1, "normal gesture must step by at most one degree");
    require(next >= state()["low_degrees"].as<int>() && next <= state()["high_degrees"].as<int>(), "motion must remain in calibrated range");
    previous = next;
  }
  require(!RobotInteraction::busy() && ticks <= 62, "bounded gesture must finish within 2.5 seconds at normal polling");
  require(RobotServos::angle(channel) == original, "gesture must restore original instructed angle");
}
}
int main(int argc, char **argv) {
  try {
    require(RobotServos::begin(), "servo setup must initialize");
    const size_t bootWrites = servoWrites.size();
    require(RobotInteraction::begin(), "empty NVS must initialize interaction");
    require(servoWrites.size() == bootWrites && interactionNvs.empty(), "begin must not move motors or persist defaults");
    require(!state()["enabled"].as<bool>() && !RobotInteraction::onReply(0), "default disabled must suppress gestures");
    RobotInteraction::poll();require(servoWrites.size() == bootWrites, "disabled polling must not emit PWM");
    require(RobotInteraction::configure(config()), "valid calibrated configuration must save");
    require(interactionNvs.size() == 1 && interactionNvsWrites == 1, "configuration must be one atomic NVS JSON entry");
    const auto stored = interactionNvs;
    require(!RobotInteraction::configure(config(true, 0)), "invalid channel must fail");
    require(!RobotInteraction::configure(config(true, 5)), "out of range channel must fail");
    require(!RobotInteraction::configure(config(true, 1, 30, 20)), "reversed bounds must fail");
    require(!RobotInteraction::configure(config(true, 1, 20, 20)), "zero range must fail");
    require(!RobotInteraction::configure(config(true, 1, 0, 31)), "range wider than 30 must fail");
    require(!RobotInteraction::configure(config(true, 1, 160, 181)), "angles beyond 180 must fail");
    require(interactionNvs == stored, "invalid configuration must not write NVS");
    failInteractionWrite = true;
    require(!RobotInteraction::configure(config(false)), "NVS failure must reject update");failInteractionWrite = false;
    require(state()["enabled"].as<bool>() && interactionNvs == stored, "failed update must preserve current and persisted config");
    const size_t beforeReload = servoWrites.size();
    require(RobotInteraction::begin() && state()["enabled"].as<bool>(), "reboot must reload valid config");
    require(servoWrites.size() == beforeReload && !RobotInteraction::busy(), "reload must not move automatically");
    require(!RobotInteraction::onReply(1) && !RobotInteraction::onReply(5), "neutral/other moods must not move");
    RobotServos::release();const size_t offWrites = servoWrites.size();
    require(!RobotInteraction::onReply(0) && servoWrites.size() == offWrites, "released PWM must never be re-enabled by gesture");
    require(RobotServos::setAngle(1, 80), "manual angle outside envelope must set");
    const size_t outsideWrites = servoWrites.size();
    require(!RobotInteraction::onReply(0) && servoWrites.size() == outsideWrites, "outside angle must skip gesture without moving");
    require(RobotServos::setAngle(1, 10), "manual calibrated initial angle must set");
    const uint8_t otherAngles[] = {RobotServos::angle(2), RobotServos::angle(3), RobotServos::angle(4)};
    require(RobotInteraction::onReply(0), "greeting must schedule gesture");
    const size_t startWrites = servoWrites.size();RobotInteraction::poll();advance(39);
    require(servoWrites.size() == startWrites, "gesture must wait 40 milliseconds before one-degree step");
    advance(1);require(RobotServos::angle(1) == 11, "first tick must move one degree toward high");
    require(!RobotInteraction::onReply(45), "duplicate trigger must not overlap gesture");
    require(!RobotInteraction::configure(config(true, 2)), "configuration must lock during motion");
    complete(1, 10);
    require(RobotServos::angle(2) == otherAngles[0] && RobotServos::angle(3) == otherAngles[1] && RobotServos::angle(4) == otherAngles[2], "gesture must command only selected channel");
    require(RobotInteraction::onReply(4), "encouragement must start gesture");advance();advance();
    RobotInteraction::stop();require(!RobotInteraction::busy() && RobotServos::angle(1) == 10, "cancel must restore original angle");
    require(RobotInteraction::onReply(45), "celebration must start gesture");advance();
    RobotInteraction::stop(false);require(RobotServos::angle(1) == 11 && !RobotInteraction::busy(), "stop without restore must leave commanded position");
    require(RobotServos::setAngle(1, 10) && RobotInteraction::onReply(0), "manual override case must start");advance();
    require(RobotServos::setAngle(1, 7), "manual command must set override");const size_t manualWrites = servoWrites.size();
    advance();RobotInteraction::stop();
    require(!RobotInteraction::busy() && RobotServos::angle(1) == 7 && servoWrites.size() == manualWrites, "manual target override must cancel without restoring old angle");
    require(RobotInteraction::onReply(0), "release cancellation case must start");advance();RobotServos::release();const size_t releaseWrites = servoWrites.size();
    advance();RobotInteraction::stop();require(!RobotInteraction::busy() && !RobotServos::outputActive(1) && servoWrites.size() == releaseWrites, "released outputs must not be restored/reactivated");
    require(RobotServos::setAngle(1, 0) && RobotInteraction::onReply(0), "timeout test must start");advance();
    advance(5000);require(!RobotInteraction::busy() && RobotServos::angle(1) == 0, "stalled poll must stop and restore within next call");
    require(RobotInteraction::configure(config(true, 1, 0, 30)), "maximum safe range must save");
    require(RobotInteraction::onReply(0), "range boundary gesture must start");complete(1, 0);
    require(RobotInteraction::configure(config(true, 3, 150, 180)), "third servo range must save");
    require(RobotServos::home() && RobotInteraction::onReply(45), "third default 180 angle must start safely inside range");complete(3, 180);
    require(RobotInteraction::onReply(0), "write failure case must start");advance();fakePwmFailure = true;advance();fakePwmFailure = false;
    require(!RobotInteraction::busy() && !RobotServos::ready() && !RobotServos::outputActive(3), "PWM failure must cancel and respect closed outputs");
    require(String(RobotInteraction::lastError()).length() > 0, "PWM failure must surface readable error");
    require(RobotServos::begin(), "hardware reinitialization must restore module");
    require(!RobotServos::outputActive(0) && !RobotServos::outputActive(5), "getter must reject invalid channels");
    require(RobotInteraction::configure(config(false)), "disabling must persist");
    require(!RobotInteraction::onReply(0), "disabled config must suppress trigger");
    interactionNvs["interaction/config"] = "{\"version\":1,\"enabled\":true,\"channel\":1,\"low_degrees\":0,\"high_degrees\":200}";
    require(!RobotInteraction::begin() && !state()["enabled"].as<bool>(), "corrupt persisted bounds must default disabled");
    require(RobotInteraction::configure(config()), "explicit valid config must repair corrupt entry");
    failInteractionOpen = true;require(!RobotInteraction::begin() && !RobotInteraction::onReply(0), "unavailable NVS must not allow motor gestures");failInteractionOpen = false;
    String html;InteractionWeb::appendControls(html);if (argc > 1) { std::ofstream file(argv[1]);file << html.c_str(); }
    std::cout << "PASS: " << checks << " interaction checks using real servo PWM commands, calibrated smooth motion, cancel/override and atomic configuration.\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n';return 1; }
}
