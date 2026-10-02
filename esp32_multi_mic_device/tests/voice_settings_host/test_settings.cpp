#include "Arduino.h"
#include "Preferences.h"
#include "../../voice_capture_settings.h"
#include "../../web_voice_ui.h"
#include "../../web_dialogue_ui.h"
#include "../../web_usage_ui.h"
#include "../../web_admin_ui.h"
#include "../../web_admin.h"
#include "../../web_response_stream.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
size_t checks = 0;
void require(bool value, const char *message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}

bool equal(const VoiceCapture::Settings &left, const VoiceCapture::Settings &right) {
  return left.gain == right.gain && left.sensitivity == right.sensitivity &&
      left.noiseSuppression == right.noiseSuppression && left.silenceMs == right.silenceMs &&
      left.listenSeconds == right.listenSeconds && left.continuous == right.continuous;
}

bool parse(const char *json, VoiceCapture::Settings &candidate) {
  JsonDocument document;
  require(!deserializeJson(document, json), "test JSON must be valid");
  return VoiceCapture::parse(document.as<JsonVariantConst>(), candidate);
}

void reject(const char *json, const VoiceCapture::Settings &starting) {
  auto candidate = starting;
  const auto live = VoiceCapture::settings();
  const auto stored = captureNvs;
  const size_t writes = captureNvsWriteCount;
  require(!parse(json, candidate), "invalid input accepted");
  require(equal(candidate, starting), "failed parse modified candidate");
  require(equal(VoiceCapture::settings(), live), "parse modified live settings");
  require(captureNvs == stored && captureNvsWriteCount == writes,
          "parse modified persistent settings");
}

void testDefaultsAndParsing() {
  captureNvs.clear();
  require(VoiceCapture::begin(), "empty storage must use defaults");
  const VoiceCapture::Settings defaults;
  require(equal(VoiceCapture::settings(), defaults), "wrong defaults");
  require(captureNvs.empty(), "reading defaults must not write NVS");

  auto candidate = defaults;
  require(parse("{\"mic_gain\":12,\"mic_sensitivity\":100,\"noise_suppression\":false,"
                "\"silence_ms\":3000,\"listen_seconds\":20,\"continuous_conversation\":true}",
                candidate), "valid maximum settings rejected");
  require(candidate.gain == 12 && candidate.sensitivity == 100 &&
          !candidate.noiseSuppression && candidate.silenceMs == 3000 &&
          candidate.listenSeconds == 20 && candidate.continuous,
          "parsed maximum values lost");
  require(equal(VoiceCapture::settings(), defaults), "parse changed current settings");
  const auto maximum = candidate;
  require(parse("{\"noise_suppression\":true,\"continuous_conversation\":false}", candidate),
          "boolean false rejected");
  require(candidate.gain == maximum.gain && candidate.silenceMs == maximum.silenceMs &&
          candidate.noiseSuppression && !candidate.continuous,
          "omitted fields or false boolean not preserved");
  const auto partial = candidate;
  require(parse("{}", candidate) && equal(candidate, partial), "empty update changed values");
  require(parse("{\"mic_gain\":1,\"mic_sensitivity\":0,\"silence_ms\":600,\"listen_seconds\":3}",
                candidate), "valid minimum settings rejected");
  require(candidate.gain == 1 && candidate.sensitivity == 0 &&
          candidate.silenceMs == 600 && candidate.listenSeconds == 3,
          "parsed minimum values lost");

  const char *invalid[] = {
      "null", "[]", "1", "\"settings\"",
      "{\"mic_gain\":0}", "{\"mic_gain\":13}", "{\"mic_gain\":-1}",
      "{\"mic_gain\":256}", "{\"mic_gain\":5.5}", "{\"mic_gain\":5.0}",
      "{\"mic_gain\":\"5\"}", "{\"mic_gain\":true}", "{\"mic_gain\":null}",
      "{\"mic_gain\":[]}", "{\"mic_gain\":{}}",
      "{\"mic_sensitivity\":101}", "{\"mic_sensitivity\":-1}",
      "{\"mic_sensitivity\":\"50\"}", "{\"mic_sensitivity\":false}",
      "{\"silence_ms\":599}", "{\"silence_ms\":3001}",
      "{\"silence_ms\":65536}", "{\"silence_ms\":1200.5}",
      "{\"silence_ms\":\"1200\"}", "{\"listen_seconds\":2}",
      "{\"listen_seconds\":21}", "{\"listen_seconds\":8.5}",
      "{\"listen_seconds\":false}", "{\"noise_suppression\":0}",
      "{\"noise_suppression\":1}", "{\"noise_suppression\":\"true\"}",
      "{\"noise_suppression\":null}", "{\"continuous_conversation\":0}",
      "{\"continuous_conversation\":\"false\"}", "{\"continuous_conversation\":null}",
      "{\"mic_gain\":8,\"noise_suppression\":false,\"listen_seconds\":99}",
      "{\"mic_gain\":18446744073709551615}"};
  for (const char *json : invalid) reject(json, partial);
}

void testStorage() {
  const VoiceCapture::Settings desired{9, 72, false, 1700, 12, true};
  require(VoiceCapture::save(desired), "valid save failed");
  require(equal(VoiceCapture::settings(), desired), "save did not update live settings");
  require(captureNvs.size() == 1 && captureNvs.count("cloud-ai/voice_capture") == 1,
          "settings must use exactly one NVS field");
  const uint64_t packed = captureNvs.at("cloud-ai/voice_capture");
  const auto originalNvs = captureNvs;
  size_t writes = captureNvsWriteCount;
  auto changed = desired;
  changed.gain = 4;
  changed.continuous = false;
  captureNvsWriteResult = 0;
  require(!VoiceCapture::save(changed), "failed NVS write reported success");
  require(equal(VoiceCapture::settings(), desired) && captureNvs == originalNvs,
          "failed write changed settings");
  require(captureNvsWriteCount == writes + 1, "save must perform one atomic write");
  captureNvsWriteResult = 4;
  require(!VoiceCapture::save(changed), "short NVS write reported success");
  require(equal(VoiceCapture::settings(), desired), "short write changed live settings");
  captureNvsWriteResult = sizeof(uint64_t);
  captureNvsBeginFails = true;
  require(!VoiceCapture::save(changed), "failed NVS open reported success");
  require(equal(VoiceCapture::settings(), desired), "failed open changed live settings");
  captureNvsBeginFails = false;
  require(VoiceCapture::begin(), "cold boot load failed");
  require(equal(VoiceCapture::settings(), desired), "cold boot did not restore settings");

  auto invalid = desired;
  invalid.gain = 0;
  writes = captureNvsWriteCount;
  require(!VoiceCapture::save(invalid), "invalid settings saved");
  invalid = desired;
  invalid.sensitivity = 101;
  require(!VoiceCapture::save(invalid), "invalid sensitivity saved");
  invalid = desired;
  invalid.silenceMs = 3001;
  require(!VoiceCapture::save(invalid), "invalid silence duration saved");
  invalid = desired;
  invalid.listenSeconds = 2;
  require(!VoiceCapture::save(invalid), "invalid listen duration saved");
  require(captureNvsWriteCount == writes && captureNvs == originalNvs,
          "validation failure wrote storage");

  // Preserve a valid schema while corrupting each field, plus unknown/reserved bits.
  const uint64_t corrupt[] = {
      0, UINT64_MAX, packed ^ (uint64_t{1} << 56), packed | (uint64_t{1} << 20),
      packed & ~uint64_t{0xff}, (packed & ~uint64_t{0xff}) | 13,
      (packed & ~(uint64_t{0xff} << 8)) | (uint64_t{101} << 8),
      (packed & ~(uint64_t{0xffff} << 24)) | (uint64_t{599} << 24),
      (packed & ~(uint64_t{0xff} << 40)) | (uint64_t{21} << 40)};
  for (uint64_t raw : corrupt) {
    captureNvs["cloud-ai/voice_capture"] = raw;
    require(!VoiceCapture::begin(), "corrupt stored settings accepted");
    require(equal(VoiceCapture::settings(), VoiceCapture::Settings{}),
            "corrupt storage did not select defaults");
    require(captureNvs["cloud-ai/voice_capture"] == raw,
            "loading corrupt storage unexpectedly overwrote NVS");
  }
  captureNvs["cloud-ai/voice_capture"] = packed;
  require(VoiceCapture::begin(), "recovery load failed");
  captureNvsBeginFails = true;
  require(!VoiceCapture::begin(), "unavailable storage reported success");
  require(equal(VoiceCapture::settings(), VoiceCapture::Settings{}),
          "unavailable storage did not select defaults");
  captureNvsBeginFails = false;
  require(VoiceCapture::begin(), "final cold boot failed");

  JsonDocument status;
  VoiceCapture::appendStatus(status.to<JsonObject>());
  require(status.size() == 6, "wrong status field count");
  require(status["mic_gain"].as<unsigned>() == desired.gain &&
          status["mic_sensitivity"].as<unsigned>() == desired.sensitivity &&
          status["noise_suppression"].is<bool>() &&
          status["noise_suppression"].as<bool>() == desired.noiseSuppression &&
          status["silence_ms"].as<unsigned>() == desired.silenceMs &&
          status["listen_seconds"].as<unsigned>() == desired.listenSeconds &&
          status["continuous_conversation"].is<bool>() &&
          status["continuous_conversation"].as<bool>() == desired.continuous,
          "status values do not match stored settings");
}

void writeUiFixture(const char *path) {
  String controls;
  VoiceWeb::appendControls(controls);
  const std::string html = controls.c_str();
  require(html.find("id='capture-form'") != std::string::npos, "capture form missing");
  require(html.find("id='capture-start'") != std::string::npos &&
          html.find("id='capture-finish'") != std::string::npos,
          "web manual recording controls missing");
  require(html.find("value='9'") != std::string::npos, "persisted gain not rendered");
  require(html.find("value='72'") != std::string::npos, "persisted sensitivity not rendered");
  require(html.find("value='1700'") != std::string::npos, "persisted pause not rendered");
  require(html.find("id='capture-noise' name='noise_suppression' type='checkbox' checked") ==
          std::string::npos, "false noise setting rendered as checked");
  require(html.find("id='capture-continuous' name='continuous_conversation' type='checkbox' checked") !=
          std::string::npos, "true continuous setting not rendered as checked");
  require(html.find("<fieldset") == std::string::npos &&
          html.find("class='card'") == std::string::npos, "controls must be unframed");
  DialogueWeb::appendControls(controls);
  UsageWeb::append(controls);
  require(std::string(controls.c_str()).find("id='usage-load'") != std::string::npos,
          "device usage controls must be rendered");
  const std::string dialogueHtml = controls.c_str();
  require(dialogueHtml.find("id='ai-search' type='checkbox'>") != std::string::npos,
          "search checkbox must default to off");
  require(dialogueHtml.find("id='ai-fields' disabled") != std::string::npos,
          "must load current cloud profile before saving");
  WebServer server;
  WebAdmin::PageStream streamed(server);
  DialogueWeb::appendControls(streamed);
  UsageWeb::append(streamed);
  streamed.finish();
  require(server.html.find("id='ai-form'") != std::string::npos,
          "dialogue controls must support production PageStream");
  require(server.html.find("/ai/usage?period=") != std::string::npos,
          "usage controls must support production PageStream");
  require(server.html.find("费用（人民币）") != std::string::npos &&
          server.html.find("Object.entries") == std::string::npos,
          "device fees must use display-ready CNY amounts only");
  WebServer proxy;
  WebAdmin::ResponseStream<WebServer> response(proxy, 5);
  const uint8_t block[] = {'1', '2', '3', '4'};
  require(response.write(block, 4) == 4 && response.write('5') == 1 && proxy.html == "12345",
          "usage proxy must stream chunks without retaining a full response");
  require(response.write('6') == 0 && response.failed() && response.getWriteError(),
          "usage proxy must fail at its response size limit");
  response.clearWriteError();
  require(response.write('6') == 0 && proxy.html == "12345",
          "HTTPClient retries must not bypass the stream limit");
  require(response.available() == 0 && response.read() == -1 && response.peek() == -1,
          "response sink must not offer readable data");
  std::ofstream out(path, std::ios::binary);
  require(static_cast<bool>(out), "fixture path cannot be opened");
  out << ADMIN_PAGE_HEAD
      << "<section class='panel' data-panel='voice'><h2>\u8bed\u97f3\u4e0e\u97f3\u91cf</h2>"
      << controls.c_str() << "</section><script>async function adminFetch(url,options){return fetch(url,options);}window.activeAdminPanel='voice';"
      << "document.querySelector('[data-nav=voice]').setAttribute('aria-current','page');"
      << "fetch('/status').then(r=>r.json()).then(window.updateVoiceCaptureStatus);"
      << "</script></main></div></body></html>";
  require(static_cast<bool>(out), "fixture write failed");
}
}  // namespace

int main(int argc, char **argv) {
  try {
    testDefaultsAndParsing();
    testStorage();
    writeUiFixture(argc > 1 ? argv[1] : "capture.html");
    std::cout << "PASS: " << checks << " capture settings checks; generated production UI fixture\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
