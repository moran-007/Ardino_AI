#include "Arduino.h"
#include "Preferences.h"
#include "../../focus_timer.h"
#include "../../web_focus_ui.h"
#include <ArduinoJson.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

size_t checks = 0;
void require(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
JsonDocument state() {
  JsonDocument doc;
  require(!deserializeJson(doc, FocusTimer::toJson()), "state must be JSON");
  return doc;
}
unsigned remaining() { return state()["remaining_seconds"].as<unsigned>(); }
void fresh() {
  fakeNvsStrings.clear();
  failNvsWrite = false;
  fakeMillis = 10000;
  require(FocusTimer::begin(), "fresh initialization must succeed");
}
void expectPhase(const char *phase, const char *status) {
  auto doc = state();
  require(doc["phase"].as<String>() == phase, "phase incorrect");
  require(doc["status"].as<String>() == status, "running/paused state incorrect");
}
int main(int argc, char **argv) {
  try {
    fresh();
    const auto result = FocusTimer::handleNaturalLanguage("开始专注二十五分钟");
    require(result.handled && result.changed && FocusTimer::focusing(),
            "spoken focus command must start a real 25 minute session");
    require(remaining() == 1500, "25 minute countdown must start at 1500 seconds");
    require(FocusTimer::active(), "running timer active");
    String announcement;
    fakeMillis += 12345;
    require(!FocusTimer::poll(announcement) && remaining() == 1488, "elapsed seconds countdown");
    require(FocusTimer::control("pause").changed, "pause command");
    expectPhase("focus", "paused");
    require(!FocusTimer::focusing() && FocusTimer::active(), "paused timer must be active but not focusing");
    fakeMillis += 600000;
    require(!FocusTimer::poll(announcement) && remaining() == 1488, "pause must freeze countdown");
    require(FocusTimer::control("resume").changed, "resume command");
    fakeMillis += 1000;
    FocusTimer::poll(announcement);
    require(remaining() == 1487, "resume must use remaining time");
    require(FocusTimer::control("stop").changed, "stop command");
    expectPhase("idle", "idle");
    require(!FocusTimer::active() && remaining() == 0, "stop must clear active countdown");

    fresh();
    FocusTimer::Config config;
    config.focusMinutes = 1;
    config.shortBreakMinutes = 1;
    config.longBreakMinutes = 2;
    config.roundsBeforeLongBreak = 2;
    require(FocusTimer::control("start", &config).changed, "configured session must start");
    const uint32_t phaseStart = fakeMillis;
    fakeMillis += 60000;
    require(FocusTimer::poll(announcement) && announcement.indexOf("休息") >= 0, "focus end must announce break");
    expectPhase("short_break", "running");
    require(state()["completed_sessions"].as<unsigned>() == 1 && remaining() == 60, "focus completion increments count");
    require(!FocusTimer::poll(announcement), "transition announcement must be consumed once");
    fakeMillis += 60000;
    require(FocusTimer::poll(announcement) && announcement.indexOf("专注") >= 0, "break end must announce focus");
    expectPhase("focus", "running");
    fakeMillis += 60000;
    require(FocusTimer::poll(announcement), "second focus completion");
    expectPhase("long_break", "running");
    require(remaining() == 120 && state()["completed_sessions"].as<unsigned>() == 2, "long break follows configured rounds");
    require(FocusTimer::control("skip").changed, "skip long break");
    expectPhase("focus", "running");
    require(state()["cycle_sessions"].as<unsigned>() == 0, "long break resets cycle");
    require(FocusTimer::control("skip").changed, "skip focus");
    expectPhase("short_break", "running");
    require(state()["completed_sessions"].as<unsigned>() == 2, "skipped focus must not earn completion");
    require(FocusTimer::control("pause").changed && FocusTimer::control("skip").changed, "skip paused break");
    expectPhase("focus", "paused");
    require(fakeMillis > phaseStart, "test clock advanced");

    fresh();
    require(FocusTimer::control("start").changed, "default start");
    fakeMillis += 61000;
    require(!FocusTimer::poll(announcement), "checkpoint should not announce");
    const unsigned checkpointRemaining = remaining();
    fakeMillis += 999000;
    require(FocusTimer::begin(), "reboot load");
    expectPhase("focus", "paused");
    require(remaining() == checkpointRemaining, "reboot must restore saved progress without counting power-off");
    fakeMillis += 50000;
    require(remaining() == checkpointRemaining, "restored countdown frozen");
    require(FocusTimer::control("resume").changed, "resume after reboot");
    require(FocusTimer::control("start").handled && !FocusTimer::control("start").changed, "repeated start must not reset timer");

    fresh();
    failNvsWrite = true;
    auto failed = FocusTimer::control("start", &config);
    require(failed.handled && !failed.changed && !FocusTimer::active(), "failed start must roll back");
    require(failed.message.indexOf("保存失败") >= 0, "failed start must not confirm success");
    require(FocusTimer::getConfig().focusMinutes == 25, "failed config save must roll back");
    failNvsWrite = false;
    require(FocusTimer::control("start", &config).changed, "recover after failure");
    failNvsWrite = true;
    require(!FocusTimer::control("pause").changed, "failed pause cannot be falsely confirmed");
    expectPhase("focus", "running");
    require(!FocusTimer::control("stop").changed && FocusTimer::active(), "failed stop rolls back");
    fakeMillis += 60000;
    require(FocusTimer::poll(announcement) && announcement.indexOf("保存失败") >= 0, "failed transition reports error");
    expectPhase("focus", "running");
    require(state()["completed_sessions"].as<unsigned>() == 0, "failed transition does not credit completion");
    require(!FocusTimer::poll(announcement), "storage failure must not repeatedly announce");
    failNvsWrite = false;
    fakeMillis += 5000;
    require(FocusTimer::poll(announcement), "failed transition can recover");
    expectPhase("short_break", "running");

    fresh();
    fakeMillis = UINT32_MAX - 10000;
    require(FocusTimer::control("start", &config).changed, "start before millis wrap");
    fakeMillis += 20000;
    require(!FocusTimer::poll(announcement) && remaining() == 40, "millis wrap must preserve elapsed time");
    fakeMillis += 40000;
    require(FocusTimer::poll(announcement), "wrapped countdown ends exactly");
    expectPhase("short_break", "running");

    fresh();
    FocusTimer::Config invalid = config;
    invalid.focusMinutes = 0;
    require(!FocusTimer::validConfig(invalid) && !FocusTimer::control("start", &invalid).changed, "zero duration invalid");
    invalid = config; invalid.roundsBeforeLongBreak = 13;
    require(!FocusTimer::validConfig(invalid), "too many rounds invalid");
    invalid = config; invalid.shortBreakMinutes = 61;
    require(!FocusTimer::validConfig(invalid), "oversized short break invalid");
    require(!FocusTimer::handleNaturalLanguage("帮我记一下开始专注二十五分钟").handled, "note text cannot execute timer");
    require(!FocusTimer::handleNaturalLanguage("番茄钟怎么用").handled, "question cannot execute timer");
    for (const char *text : {"开始专注0分钟", "开始专注121分钟", "开始专注二百分钟", "开始专注-1分钟", "开始专注25.5分钟"}) {
      auto rejected = FocusTimer::handleNaturalLanguage(text);
      require(rejected.handled && !rejected.changed && !FocusTimer::active(), "invalid spoken duration must be rejected");
    }
    require(FocusTimer::handleNaturalLanguage("请开始专注一百二十分钟。").changed && remaining() == 7200, "Chinese 120 minute duration");
    require(FocusTimer::handleNaturalLanguage("暂停专注").changed, "spoken pause");
    require(FocusTimer::handleNaturalLanguage("继续专注").changed, "spoken resume");
    require(FocusTimer::handleNaturalLanguage("专注状态").handled, "spoken state query");
    require(FocusTimer::handleNaturalLanguage("结束专注").changed, "spoken stop");
    require(FocusTimer::handleNaturalLanguage("开始番茄钟").changed, "spoken default pomodoro start");
    require(FocusTimer::displaySummary().indexOf("专注") >= 0, "display summary must show focus");

    fresh();
    require(FocusTimer::control("start", &config).changed, "start config transition test");
    fakeMillis += 10000;
    auto revised = config;
    revised.focusMinutes = 3;
    revised.shortBreakMinutes = 2;
    require(FocusTimer::control("configure", &revised).changed, "configure while running");
    require(remaining() == 50 && state()["phase_total_seconds"].as<unsigned>() == 60,
            "changing config must not reset current phase");
    fakeMillis += 50000;
    require(FocusTimer::poll(announcement) && remaining() == 120, "new break configuration must take effect next phase");
    fakeMillis += 120000;
    require(FocusTimer::poll(announcement) && remaining() == 180, "new focus configuration must take effect next phase");
    require(FocusTimer::begin(), "break/focus reboot state");
    expectPhase("focus", "paused");
    require(state()["completed_sessions"].as<unsigned>() == 1 && FocusTimer::getConfig().focusMinutes == 3,
            "completion and configuration survive reboot");
    require(FocusTimer::control("stop").changed && FocusTimer::begin(), "stopped state reboot");
    expectPhase("idle", "idle");
    require(state()["completed_sessions"].as<unsigned>() == 1, "lifetime completions survive stop/reboot");

    fresh();
    require(FocusTimer::control("start", &config).changed, "start delayed poll test");
    fakeMillis += 1200000;
    require(FocusTimer::poll(announcement) && remaining() == 60 && state()["completed_sessions"].as<unsigned>() == 1,
            "delayed poll must begin a fresh break and never fabricate missed focus rounds");
    require(!FocusTimer::poll(announcement), "delayed transition emitted once");

    fresh();
    require(FocusTimer::control("start").changed, "default cycle start");
    for (unsigned round = 1; round <= 4; ++round) {
      fakeMillis += remaining() * 1000U;
      require(FocusTimer::poll(announcement), "default focus transition");
      expectPhase(round == 4 ? "long_break" : "short_break", "running");
      require(remaining() == (round == 4 ? 900U : 300U), "default break duration");
      require(state()["completed_sessions"].as<unsigned>() == round, "default completed count");
      fakeMillis += remaining() * 1000U;
      require(FocusTimer::poll(announcement), "default break transition");
      expectPhase("focus", "running");
      require(remaining() == 1500, "default next focus duration");
    }
    require(state()["cycle_sessions"].as<unsigned>() == 0, "default four round cycle reset");

    fresh();
    require(FocusTimer::control("start").changed, "start boot failure test");
    const auto savedRecord = fakeNvsStrings.begin()->second;
    failNvsWrite = true;
    require(!FocusTimer::begin(), "boot pause persist failure must be reported");
    expectPhase("focus", "paused");
    require(state()["storage_error"].as<bool>(), "boot pause failure exposed");
    failNvsWrite = false;
    require(FocusTimer::control("resume").changed, "boot persist failure can be recovered by resume");
    auto corrupt = state();
    deserializeJson(corrupt, savedRecord.c_str());
    corrupt["remaining_ms"] = 7200001;
    String corruptRecord;
    serializeJson(corrupt, corruptRecord);
    fakeNvsStrings.begin()->second = corruptRecord.c_str();
    require(!FocusTimer::begin() && !FocusTimer::active(), "corrupt duration record rejected without partial state");
    require(!state()["initialized"].as<bool>() && !FocusTimer::control("start").changed,
            "corrupt storage cannot be silently overwritten");
    require(fakeNvsStrings.begin()->second == corruptRecord.c_str(), "corrupt record retained for recovery");

    String page("<!doctype html><meta charset='utf-8'><main>");
    FocusWeb::appendControls(page);
    page += "</main>";
    require(page.indexOf("/focus") >= 0 && page.indexOf("focus-status") >= 0, "focus UI routes/status");
    if (argc > 1) std::ofstream(argv[1]) << page.c_str();
    std::cout << checks << " focus host checks passed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
