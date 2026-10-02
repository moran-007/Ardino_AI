#include "robot_display.h"
#include "robot_photo.h"
#include "robot_avatar.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <algorithm>
#include <atomic>
#include <math.h>
#include <new>
#include <string.h>
#include <time.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <Preferences.h>
#include <qrcode.h>

namespace RobotDisplay {
namespace {

constexpr uint16_t COLOR_BG = 0x0863;
constexpr uint16_t COLOR_PANEL = 0x10C5;
constexpr uint16_t COLOR_TEXT = ST77XX_WHITE;
constexpr uint16_t COLOR_MUTED = 0x8410;
constexpr uint16_t COLOR_CYAN = 0x05FF;
constexpr uint16_t COLOR_GREEN = 0x07E0;
constexpr uint16_t COLOR_YELLOW = 0xFFE0;
constexpr uint16_t COLOR_RED = 0xF800;
constexpr uint16_t COLOR_PURPLE = 0xF81F;
constexpr uint32_t ACTIVE_FRAME_INTERVAL_MS = 80;
constexpr uint32_t IDLE_FRAME_INTERVAL_MS = 160;
constexpr uint32_t TEXT_PAGE_INTERVAL_MS = 2800;
constexpr uint32_t IDLE_CAROUSEL_INTERVAL_MS = 6500;
constexpr uint32_t BATTERY_SAMPLE_INTERVAL_MS = 10000;
// Native landscape framebuffer sizes. Runtime panel selection comes from NVS.
constexpr int ST7789_LANDSCAPE_WIDTH = 320;
constexpr int ST7789_LANDSCAPE_HEIGHT = 240;
constexpr int ST7735_LANDSCAPE_WIDTH = 160;
constexpr int ST7735_LANDSCAPE_HEIGHT = 128;
constexpr char DISPLAY_PREF_NAMESPACE[] = "cloud-ai";
constexpr char DISPLAY_PREF_KEY[] = "disp_panel";
constexpr char ROTATION_PREF_KEY[] = "disp_rot";
// Percent + mode in a single NVS value, so a partial write cannot split them.
constexpr char BRIGHTNESS_PREF_KEY[] = "disp_light";
constexpr uint16_t BRIGHTNESS_POT_FLAG = 0x100;
std::atomic<uint16_t> brightnessConfig{100};
std::atomic<uint8_t> appliedBrightness{100};
std::atomic<bool> backlightPwm{false};
constexpr char VOICE_LAYOUT_PREF_KEY[] = "disp_voice";
constexpr char EXPRESSIONS_PREF_KEY[] = "disp_expr";
constexpr char BODY_PREF_KEY[] = "disp_body";
std::atomic<bool> portraitBodyConfig{false};
std::atomic<uint8_t> voiceLayoutConfig{0};  // bit 0: button, bit 1: wake

Panel activePanel = DEFAULT_PANEL;
bool highResPanel = true;
uint8_t activeRotation = ST7789_ROTATION;
bool portrait = false;
int screenWidth = ST7789_LANDSCAPE_WIDTH;
int screenHeight = ST7789_LANDSCAPE_HEIGHT;
int headerHeight = 28;
int textMargin = 12;
int textLineHeight = 22;
uint16_t scanline[ST7789_LANDSCAPE_WIDTH];

void configurePanelGeometry(Panel panel, uint8_t rotation) {
  activePanel = panel;
  activeRotation = rotation;
  portrait = (rotation & 1U) == 0;
  highResPanel = panel == Panel::M200T_ST7789_240X320_7PIN;
  const int longSide = highResPanel ? ST7789_LANDSCAPE_WIDTH : ST7735_LANDSCAPE_WIDTH;
  const int shortSide = highResPanel ? ST7789_LANDSCAPE_HEIGHT : ST7735_LANDSCAPE_HEIGHT;
  screenWidth = portrait ? shortSide : longSide;
  screenHeight = portrait ? longSide : shortSide;
  headerHeight = highResPanel ? 28 : 18;
  textMargin = highResPanel ? 12 : 5;
  textLineHeight = highResPanel ? 22 : 15;
}

Panel loadPanelPreference() {
  Preferences storage;
  Panel panel = DEFAULT_PANEL;
  if (storage.begin(DISPLAY_PREF_NAMESPACE, true)) {
    const String stored = storage.getString(DISPLAY_PREF_KEY, panelId(DEFAULT_PANEL));
    storage.end();
    Panel parsed;
    if (panelFromId(stored, parsed)) panel = parsed;
  }
  return panel;
}

uint8_t loadRotationPreference(Panel panel) {
  const uint8_t fallback = panel == Panel::M200T_ST7789_240X320_7PIN
                               ? ST7789_ROTATION : ST7735_ROTATION;
  Preferences storage;
  uint8_t rotation = fallback;
  if (storage.begin(DISPLAY_PREF_NAMESPACE, true)) {
    rotation = storage.getUChar(ROTATION_PREF_KEY, fallback);
    storage.end();
  }
  return rotation <= 3 ? rotation : fallback;
}

struct Model {
  Scene scene = Scene::Booting;
  String title = "启动中";
  String body;
  String question;
  String answer;
  String city = "上海";
  float temperatureC = 0.0f;
  String weatherCondition;
  bool weatherValid = false;
  String weatherStatus = "天气获取中";
  String forecastDay[3];
  String forecastCondition[3];
  int forecastHighC[3] = {0, 0, 0};
  int forecastLowC[3] = {0, 0, 0};
  size_t forecastCount = 0;
  String alarmStatus = "暂无闹钟";
  String noteText;
  String todoText;
  size_t noteCount = 0;
  size_t todoCount = 0;
  size_t pendingTodoCount = 0;
  uint8_t pageMask = DEFAULT_PAGES;
  bool photoMode = false;
  bool networkConnected = false;
  int rssi = -127;
  char ip[16] = {};
  bool provisioning = false;
  bool phoneConnected = false;
  String setupSsid;
  String setupPassword;
  String setupUrl;
  bool voiceFromWake = false;
  bool voiceActive = false;
  uint8_t dialogueExpression = 0;
  bool hasDialogueExpression = false;
  bool focusActive = false;
  String focusStatus;
  uint64_t expressions = RobotExpressions::ALL;
  uint8_t previewId = 0;
  uint32_t previewUntilMs = 0;
  uint32_t revision = 1;
};

class DisplayCanvas16 : public Adafruit_GFX {
 public:
  DisplayCanvas16(int16_t width, int16_t height, bool requirePsram)
      : Adafruit_GFX(width, height), buffer_(nullptr), inPsram_(false),
        clipLeft_(0), clipTop_(0), clipRight_(width), clipBottom_(height) {
    const size_t bytes = static_cast<size_t>(width) * height * sizeof(uint16_t);
#if CONFIG_IDF_TARGET_ESP32S3
    buffer_ = static_cast<uint16_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    inPsram_ = buffer_ != nullptr;
    if (!buffer_ && !requirePsram) {
      buffer_ = static_cast<uint16_t *>(
          heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
#else
    (void)requirePsram;
    buffer_ = static_cast<uint16_t *>(malloc(bytes));
#endif
    if (buffer_) memset(buffer_, 0, bytes);
  }

  ~DisplayCanvas16() {
    free(buffer_);
    buffer_ = nullptr;
  }

  uint16_t *getBuffer() const { return buffer_; }
  bool inPsram() const { return inPsram_; }

  void setClipRect(int16_t x, int16_t y, int16_t w, int16_t h) {
    clipLeft_ = max(0, static_cast<int>(x));
    clipTop_ = max(0, static_cast<int>(y));
    clipRight_ = min(static_cast<int>(WIDTH), static_cast<int>(x) + w);
    clipBottom_ = min(static_cast<int>(HEIGHT), static_cast<int>(y) + h);
  }

  void resetClip() { setClipRect(0, 0, WIDTH, HEIGHT); }

  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (!buffer_ || x < clipLeft_ || y < clipTop_ ||
        x >= clipRight_ || y >= clipBottom_) return;
    buffer_[static_cast<size_t>(y) * WIDTH + x] = color;
  }

  void drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (!buffer_ || y < clipTop_ || y >= clipBottom_ || w <= 0) return;
    if (x < clipLeft_) {
      w -= clipLeft_ - x;
      x = clipLeft_;
    }
    if (x + w > clipRight_) w = clipRight_ - x;
    if (w <= 0) return;
    uint16_t *dst = buffer_ + static_cast<size_t>(y) * WIDTH + x;
    for (int16_t i = 0; i < w; ++i) dst[i] = color;
  }

  void drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (!buffer_ || x < clipLeft_ || x >= clipRight_ || h <= 0) return;
    if (y < clipTop_) {
      h -= clipTop_ - y;
      y = clipTop_;
    }
    if (y + h > clipBottom_) h = clipBottom_ - y;
    if (h <= 0) return;
    uint16_t *dst = buffer_ + static_cast<size_t>(y) * WIDTH + x;
    for (int16_t i = 0; i < h; ++i) {
      *dst = color;
      dst += WIDTH;
    }
  }

  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h,
                uint16_t color) override {
    if (!buffer_ || w <= 0 || h <= 0) return;
    if (x < clipLeft_) { w -= clipLeft_ - x; x = clipLeft_; }
    if (y < clipTop_) { h -= clipTop_ - y; y = clipTop_; }
    if (x >= clipRight_ || y >= clipBottom_) return;
    if (x + w > clipRight_) w = clipRight_ - x;
    if (y + h > clipBottom_) h = clipBottom_ - y;
    if (w <= 0 || h <= 0) return;
    for (int16_t row = 0; row < h; ++row) {
      uint16_t *dst = buffer_ + static_cast<size_t>(y + row) * WIDTH + x;
      for (int16_t col = 0; col < w; ++col) dst[col] = color;
    }
  }

  void fillScreen(uint16_t color) {
    if (!buffer_) return;
    const size_t pixels = static_cast<size_t>(WIDTH) * HEIGHT;
    for (size_t i = 0; i < pixels; ++i) buffer_[i] = color;
  }

 private:
  uint16_t *buffer_;
  bool inPsram_;
  int16_t clipLeft_, clipTop_, clipRight_, clipBottom_;
};

SPIClass displaySpi(FSPI);
Adafruit_ST7735 tft7735(&displaySpi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Adafruit_ST7789 tft7789(&displaySpi, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Adafruit_ST77xx *tft = nullptr;
DisplayCanvas16 *frame = nullptr;
U8G2_FOR_ADAFRUIT_GFX textRenderer;
SemaphoreHandle_t modelMutex = nullptr;
SemaphoreHandle_t ioMutex = nullptr;
TaskHandle_t displayTaskHandle = nullptr;
Model model;
bool initialized = false;
volatile bool suspended = false;
float measuredBatteryVoltage = 0.0f;
int measuredBatteryPercent = -1;
size_t textPageOffset = 0;
size_t nextTextPageOffset = 0;
uint32_t lastTextPageAtMs = 0;
uint32_t lastBatterySampleAtMs = 0;
uint32_t renderedRevision = 0;
Scene renderedScene = Scene::Booting;
uint32_t idleStartedAtMs = 0;
uint32_t idlePageStartedAtMs = 0;
uint8_t lastIdleMask = 0;
uint8_t lastIdlePage = 0;
Scene lastFrameScene = Scene::Booting;
uint32_t cachedPhotoRevision = 0;
uint32_t lastPhotoAttemptAtMs = 0;
bool photoPainted = false;

bool isVoiceScene(Scene scene) {
  return scene == Scene::Listening || scene == Scene::Thinking || scene == Scene::Speaking;
}

bool usingVoicePresentation(const Model &snapshot) {
  return isVoiceScene(snapshot.scene) || (snapshot.voiceActive &&
      (snapshot.scene == Scene::Connecting || snapshot.scene == Scene::Error));
}

// Only the display task samples ADC and writes PWM. Web requests publish a
// small atomic setting; rotating a knob never writes flash.
void updateBrightness(bool setupVisible) {
  const uint16_t config = brightnessConfig.load();
  const bool pot = (config & BRIGHTNESS_POT_FLAG) != 0;
  static bool wasPot = false;
  static int filtered = -1;
  static uint8_t lastPotPercent = 100;
  uint8_t percent = config & 0xFF;
  if (pot) {
    if (!wasPot) {
      pinMode(PIN_BRIGHTNESS_POT, INPUT);
      analogSetPinAttenuation(PIN_BRIGHTNESS_POT, ADC_11db);
      filtered = -1;
    }
    int raw = 0;
    for (int i = 0; i < 4; ++i) raw += analogRead(PIN_BRIGHTNESS_POT);
    raw /= 4;
    filtered = filtered < 0 ? raw : (filtered * 3 + raw + 2) / 4;
    percent = MIN_BRIGHTNESS + (filtered * (100 - MIN_BRIGHTNESS) + 2047) / 4095;
    // Suppress a one-percent oscillation, while allowing both endpoints.
    const uint8_t previous = lastPotPercent;
    if (wasPot && abs(static_cast<int>(percent) - previous) < 2 &&
        percent != MIN_BRIGHTNESS && percent != 100) percent = previous;
    lastPotPercent = percent;
  }
  wasPot = pot;
  if (setupVisible) percent = max(percent, static_cast<uint8_t>(30));
  if (backlightPwm && !ledcWrite(PIN_TFT_BACKLIGHT, (percent * 255U + 50U) / 100U)) {
    // A failed PWM output falls back to the same software dimmer as ST7789.
    ledcDetach(PIN_TFT_BACKLIGHT);
    pinMode(PIN_TFT_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_TFT_BACKLIGHT, HIGH);
    backlightPwm = false;
  }
  appliedBrightness.store(percent);
}

// Cached QR modules, independent of the framebuffer. The bundled Espressif
// encoder allocates only when a payload changes (or after a failed retry).
struct SetupQr {
  uint8_t size = 0;
  uint8_t bits[(29 * 29 + 7) / 8] = {};
  bool black(int x, int y) const {
    const int bit = y * size + x;
    return bits[bit / 8] & (1U << (bit % 8));
  }
};
SetupQr wifiQr, urlQr;
SetupQr *qrCaptureTarget = nullptr;
String cachedSetupSsid, cachedSetupPassword, cachedSetupUrl;
uint32_t lastQrAttemptMs = 0;

void captureQr(esp_qrcode_handle_t qr) {
  const int size = esp_qrcode_get_size(qr);
  if (!qrCaptureTarget || size > 29 || size < 21) return;
  qrCaptureTarget->size = size;
  memset(qrCaptureTarget->bits, 0, sizeof(qrCaptureTarget->bits));
  for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
    if (esp_qrcode_get_module(qr, x, y)) {
      const int bit = y * size + x;
      qrCaptureTarget->bits[bit / 8] |= 1U << (bit % 8);
    }
  }
}

String escapeWifiQr(const String &value) {
  String escaped;
  escaped.reserve(value.length() * 2);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') escaped += '\\';
    escaped += c;
  }
  return escaped;
}

void encodeSetupQr(SetupQr &target, const String &payload) {
  target.size = 0;
  qrCaptureTarget = &target;
  esp_qrcode_config_t config = {captureQr, 3, ESP_QRCODE_ECC_MED};
  if (esp_qrcode_generate(&config, payload.c_str()) != ESP_OK) target.size = 0;
  qrCaptureTarget = nullptr;
}

void prepareSetupQr(const Model &snapshot) {
  if (cachedSetupSsid == snapshot.setupSsid && cachedSetupPassword == snapshot.setupPassword &&
      cachedSetupUrl == snapshot.setupUrl &&
      ((wifiQr.size && urlQr.size) || millis() - lastQrAttemptMs < 5000U)) return;
  cachedSetupSsid = snapshot.setupSsid;
  cachedSetupPassword = snapshot.setupPassword;
  cachedSetupUrl = snapshot.setupUrl;
  lastQrAttemptMs = millis();
  encodeSetupQr(wifiQr, "WIFI:T:WPA;S:" + escapeWifiQr(snapshot.setupSsid) +
                        ";P:" + escapeWifiQr(snapshot.setupPassword) + ";;");
  encodeSetupQr(urlQr, snapshot.setupUrl);
}

uint16_t sceneColor(Scene scene) {
  switch (scene) {
    case Scene::Listening: return COLOR_CYAN;
    case Scene::Thinking: return COLOR_PURPLE;
    case Scene::Speaking: return COLOR_GREEN;
    case Scene::Alarm: return COLOR_RED;
    case Scene::Error: return COLOR_PURPLE;
    case Scene::Updating: return COLOR_CYAN;
    case Scene::Connecting: return COLOR_YELLOW;
    case Scene::Booting:
    case Scene::Idle:
    default: return COLOR_CYAN;
  }
}

size_t nextUtf8Index(const String &value, size_t index) {
  if (index >= value.length()) return value.length();
  const uint8_t lead = static_cast<uint8_t>(value[index]);
  size_t count = 1;
  if ((lead & 0xE0) == 0xC0) count = 2;
  else if ((lead & 0xF0) == 0xE0) count = 3;
  else if ((lead & 0xF8) == 0xF0) count = 4;
  return min(value.length(), index + count);
}


// Reuse one small stack buffer per line. No substring allocation per character.
size_t fitTextLine(const String &value, size_t offset, int width, char (&line)[160]) {
  line[0] = 0;
  size_t end = offset, used = 0;
  while (end < value.length()) {
    if (value[end] == '\n' || value[end] == '\r') return end + 1;
    const size_t next = nextUtf8Index(value, end);
    const size_t bytes = next - end;
    if (used + bytes >= sizeof(line)) break;
    memcpy(line + used, value.c_str() + end, bytes);
    line[used + bytes] = 0;
    if (textRenderer.getUTF8Width(line) > width) { line[used] = 0; break; }
    used += bytes;
    end = next;
  }
  return end;
}

void fitLabel(const String &value, int width, char (&line)[160]) {
  const size_t end = fitTextLine(value, 0, width, line);
  if (end >= value.length()) return;
  size_t used = strlen(line);
  while (used > 0 && (used + 3 >= sizeof(line) ||
         textRenderer.getUTF8Width(line) + textRenderer.getUTF8Width("...") > width)) {
    --used;
    while (used > 0 && (static_cast<uint8_t>(line[used]) & 0xC0) == 0x80) --used;
    line[used] = 0;
  }
  if (used + 3 < sizeof(line)) strcat(line, "...");
}

void drawFittedUtf8(int x, int baseline, int width, const String &value,
                    uint16_t color = COLOR_TEXT) {
  char line[160];
  fitLabel(value, width, line);
  textRenderer.setForegroundColor(color);
  textRenderer.drawUTF8(x, baseline, line);
}

void drawUtf8(int x, int baseline, const String &value, uint16_t color = COLOR_TEXT) {
  drawFittedUtf8(x, baseline, screenWidth - x - textMargin, value, color);
}

void drawCenteredRegion(int x, int width, int baseline, const String &value,
                        uint16_t color = COLOR_TEXT) {
  char line[160];
  fitLabel(value, width, line);
  textRenderer.setForegroundColor(color);
  textRenderer.drawUTF8(x + max(0, (width - textRenderer.getUTF8Width(line)) / 2), baseline, line);
}

void drawCenteredUtf8(int baseline, const String &value, uint16_t color = COLOR_TEXT) {
  drawCenteredRegion(textMargin, screenWidth - 2 * textMargin, baseline, value, color);
}

void setTransparentFont(const uint8_t *font) {
  // U8g2_for_Adafruit_GFX resets is_transparent to 0 when the font changes.
  // Apply transparency AFTER selecting every font, including the clock/header.
  textRenderer.setFont(font);
  textRenderer.setFontMode(1);
}

void restoreBodyFont() {
  setTransparentFont(highResPanel ? u8g2_font_wqy16_t_gb2312
                                 : u8g2_font_wqy12_t_gb2312);
}

void drawScrollingUtf8(int x, int baseline, int width, const String &value,
                       uint16_t color = COLOR_TEXT) {
  const int overflow = textRenderer.getUTF8Width(value.c_str()) - width;
  int offset = 0;
  if (overflow > 0) {
    // Show both ends before wrapping. Even a long district name finishes its
    // first pass within one carousel slot. Draw complete UTF-8, clip pixels.
    // ESP32 toolchains may typedef uint32_t as unsigned long, unlike U literals.
    const uint32_t travelMs = std::min<uint32_t>(
        4200U, std::max<uint32_t>(1600U, static_cast<uint32_t>(overflow) * 40U));
    const uint32_t phase = (millis() - idlePageStartedAtMs) % (travelMs + 1800U);
    if (phase > 900U) {
      offset = static_cast<int>((static_cast<uint32_t>(overflow) *
                                std::min<uint32_t>(phase - 900U, travelMs)) / travelMs);
    }
  }
  frame->setClipRect(x, baseline - textLineHeight + 3, width, textLineHeight);
  textRenderer.setForegroundColor(color);
  textRenderer.drawUTF8(x - offset, baseline, value.c_str());
  frame->resetClip();
}

size_t drawTextPage(const String &value, size_t startOffset, int y, int maxLines) {
  if (value.isEmpty()) return 0;
  size_t offset = startOffset < value.length() ? startOffset : 0;
  char line[160];
  textRenderer.setForegroundColor(COLOR_TEXT);
  for (int row = 0; row < maxLines && offset < value.length(); ++row) {
    while (offset < value.length() &&
           (value[offset] == ' ' || value[offset] == '\n' || value[offset] == '\r')) ++offset;
    if (offset >= value.length()) break;
    const size_t end = fitTextLine(value, offset, screenWidth - textMargin * 2, line);
    textRenderer.drawUTF8(textMargin, y + row * textLineHeight, line);
    offset = end > offset ? end : nextUtf8Index(value, offset);
  }
  return offset >= value.length() ? 0 : offset;
}

void localClock(char *output, size_t capacity, const char *format) {
  struct tm value {};
  if (!getLocalTime(&value, 0)) {
    strlcpy(output, "--:--", capacity);
    return;
  }
  strftime(output, capacity, format, &value);
}

void drawClock(int x, int baseline) {
  char clock[12];
  localClock(clock, sizeof(clock), "%H:%M");
  setTransparentFont(highResPanel ? u8g2_font_logisoso32_tn : u8g2_font_helvB18_tn);
  textRenderer.setForegroundColor(COLOR_TEXT);
  if (x < 0) x = (screenWidth - textRenderer.getUTF8Width(clock)) / 2;
  textRenderer.drawUTF8(x, baseline, clock);
  restoreBodyFont();
}

void sampleBattery() {
  if (!BATTERY_MONITOR_ENABLED) {
    measuredBatteryVoltage = 0.0f;
    measuredBatteryPercent = -1;
    return;
  }
  uint32_t millivolts = 0;
  for (int i = 0; i < 12; ++i) {
    millivolts += analogReadMilliVolts(PIN_BATTERY_ADC);
    delay(2);
  }
  // 100k/100k divider: battery voltage is twice the ADC voltage.
  measuredBatteryVoltage = (millivolts / 12.0f) * 2.0f / 1000.0f;
  if (measuredBatteryVoltage < 2.8f || measuredBatteryVoltage > 4.5f) {
    measuredBatteryPercent = -1;
    return;
  }
  // Practical Li-ion approximation; it is intentionally conservative below 3.5 V.
  if (measuredBatteryVoltage >= 4.15f) measuredBatteryPercent = 100;
  else if (measuredBatteryVoltage >= 3.90f) measuredBatteryPercent = 70 + (measuredBatteryVoltage - 3.90f) * 120;
  else if (measuredBatteryVoltage >= 3.70f) measuredBatteryPercent = 35 + (measuredBatteryVoltage - 3.70f) * 175;
  else if (measuredBatteryVoltage >= 3.50f) measuredBatteryPercent = 10 + (measuredBatteryVoltage - 3.50f) * 125;
  else measuredBatteryPercent = max(0, static_cast<int>((measuredBatteryVoltage - 3.20f) * 33));
  measuredBatteryPercent = constrain(measuredBatteryPercent, 0, 100);
}

void drawHeader(const Model &snapshot) {
  frame->fillRect(0, 0, screenWidth, headerHeight, COLOR_BG);
  frame->drawFastHLine(0, headerHeight - 1, screenWidth, COLOR_PANEL);
  if (snapshot.ip[0]) {
    char label[20];
    snprintf(label, sizeof(label), "%s%s",
             highResPanel ? (snapshot.networkConnected ? "IP " : "AP ") : "", snapshot.ip);
    textRenderer.setForegroundColor(snapshot.networkConnected ? COLOR_CYAN : COLOR_YELLOW);
    // Native-size glyphs avoid the blocky doubled 5x7 bitmap font.
    setTransparentFont(highResPanel ? u8g2_font_7x14_tr : u8g2_font_6x10_tr);
    textRenderer.drawUTF8(highResPanel ? 8 : 4, highResPanel ? 20 : 13, label);
    restoreBodyFont();
  } else {
    drawFittedUtf8(highResPanel ? 8 : 4, highResPanel ? 20 : 13,
                   highResPanel ? screenWidth - 88 : 96, "Wi-Fi 未连接", COLOR_MUTED);
  }

  const int bars = !snapshot.networkConnected ? 0 :
                   snapshot.rssi >= -55 ? 4 : snapshot.rssi >= -67 ? 3 : snapshot.rssi >= -78 ? 2 : 1;
  if (highResPanel) {
    const int right = screenWidth - ST7789_LANDSCAPE_WIDTH;
    for (int i = 0; i < 4; ++i) {
      const int height = 5 + i * 4;
      frame->fillRect(right + 246 + i * 7, 23 - height, 5, height,
                      i < bars ? COLOR_CYAN : COLOR_MUTED);
    }
    frame->drawRoundRect(right + 278, 7, 34, 15, 2, COLOR_TEXT);
    frame->fillRect(right + 312, 11, 4, 7, COLOR_TEXT);
    if (measuredBatteryPercent >= 0) {
      const int fill = (28 * measuredBatteryPercent) / 100;
      const uint16_t color = measuredBatteryPercent <= 15 ? COLOR_RED : COLOR_GREEN;
      if (fill > 0) frame->fillRect(right + 281, 10, fill, 9, color);
    } else {
      drawUtf8(right + 289, 20, "?", COLOR_MUTED);
    }
  } else {
    for (int i = 0; i < 4; ++i) {
      const int height = 3 + i * 2;
      frame->fillRect(104 + i * 4, 14 - height, 3, height,
                      i < bars ? COLOR_CYAN : COLOR_MUTED);
    }
    // On the 128-pixel portrait header, keep the complete IP and Wi-Fi bars.
    if (portrait) return;
    frame->drawRect(125, 5, 27, 10, COLOR_TEXT);
    frame->fillRect(152, 8, 2, 4, COLOR_TEXT);
    if (measuredBatteryPercent >= 0) {
      const int fill = (23 * measuredBatteryPercent) / 100;
      const uint16_t color = measuredBatteryPercent <= 15 ? COLOR_RED : COLOR_GREEN;
      if (fill > 0) frame->fillRect(127, 7, fill, 6, color);
    } else {
      drawUtf8(132, 13, "?", COLOR_MUTED);
    }
  }
}

bool weatherContains(const String &condition, const char *word) {
  return condition.indexOf(word) >= 0;
}

uint8_t frameExpression = 0;  // Selected from the locked snapshot once per frame.
uint8_t idleExpression(uint32_t now, uint64_t enabled) {
  static uint8_t expression = 0;
  static uint32_t selectedAtMs = 0, holdMs = 0;
  if (holdMs == 0 || now - selectedAtMs >= holdMs || !(enabled & (uint64_t{1} << expression))) {
    expression = RobotExpressions::choose(enabled, expression, esp_random());
    selectedAtMs = now;
    holdMs = 2800U + esp_random() % 2600U;
  }
  return expression;
}

void drawAvatar(Scene scene, int x, int y, int width, int height, bool body) {
  const uint32_t now = millis();
  RobotAvatar::draw(*frame, scene, body, x, y, width, height, now, frameExpression);
}

void drawAmbientBackground(int bottom) {
  // Two quiet blue waves are drawn directly in the existing framebuffer.
  const int depth = highResPanel ? 50 : 24;
  const int firstY = bottom - depth * 4 / 3;
  uint16_t backColor[70],frontColor[70];
  for (int row = 0; row < bottom - firstY; ++row) {
    const int fade = row * 12 / (bottom - firstY);
    backColor[row] = (1U << 11) | ((5 + fade / 3) << 5) | (10 + fade / 2);
    frontColor[row] = (2U << 11) | ((9 + fade / 2) << 5) | (19 + fade / 2);
  }
  for (int x = 0; x < screenWidth; ++x) {
    const float phase = static_cast<float>(x) / screenWidth;
    const int back = bottom - depth + static_cast<int>(sinf(phase * 4.4f + 0.6f) * depth / 3);
    const int front = bottom - depth / 2 + static_cast<int>(sinf(phase * 4.8f + 2.2f) * depth / 3);
    for (int y = std::min(back, front); y < bottom; ++y)
      frame->drawPixel(x, y, y >= front ? frontColor[y-firstY] : backColor[y-firstY]);
  }
}

void drawExpression(const Model &snapshot) {
  const int strip = usingVoicePresentation(snapshot) ? (highResPanel ? 54 : 32) : 0;
  const Scene avatarScene = strip ? Scene::Idle : snapshot.scene;
  const int bottom = screenHeight - strip;
  drawAmbientBackground(bottom);
  if (portrait && portraitBodyEnabled()) {
    drawAvatar(avatarScene, textMargin, headerHeight + 4, screenWidth - textMargin * 2,
               bottom - headerHeight - 8, true);
  } else {
    char date[24];localClock(date,sizeof(date),"%m/%d %a");
    const int baseline = highResPanel ? (portrait ? 88 : 75) : 45;
    drawClock(portrait ? -1 : textMargin + 4, baseline);
    if (portrait) drawCenteredUtf8(baseline + (highResPanel ? 24 : 16), date, COLOR_MUTED);
    else drawFittedUtf8(highResPanel ? 162 : 83, baseline - 4,
                       screenWidth - (highResPanel ? 170 : 88), date, COLOR_MUTED);
    const int top = baseline + (portrait ? (highResPanel ? 35 : 21) : 9);
    drawAvatar(avatarScene, textMargin + 4, top, screenWidth - textMargin * 2 - 8,
               bottom - top - 8, false);
  }
}

void drawWeatherIcon(const String &condition, int x, int y) {
  const int s = highResPanel ? 2 : 1;
  if (weatherContains(condition, "雷")) {
    frame->fillCircle(x - 5 * s, y - 3 * s, 6 * s, COLOR_MUTED);
    frame->fillCircle(x + 3 * s, y - 4 * s, 8 * s, COLOR_MUTED);
    frame->fillRect(x - 9 * s, y - 3 * s, 20 * s, 7 * s, COLOR_MUTED);
    frame->drawLine(x + 2 * s, y + 4 * s, x - 3 * s, y + 12 * s, COLOR_YELLOW);
    frame->drawLine(x - 3 * s, y + 12 * s, x + 4 * s, y + 10 * s, COLOR_YELLOW);
    return;
  }
  if (weatherContains(condition, "雨")) {
    frame->fillCircle(x - 5 * s, y - 3 * s, 6 * s, COLOR_MUTED);
    frame->fillCircle(x + 3 * s, y - 4 * s, 8 * s, COLOR_MUTED);
    frame->fillRect(x - 9 * s, y - 3 * s, 20 * s, 7 * s, COLOR_MUTED);
    for (int i = -7; i <= 7; i += 7) {
      frame->drawLine(x + i * s, y + 6 * s, x + (i - 2) * s, y + 11 * s, COLOR_CYAN);
    }
    return;
  }
  if (weatherContains(condition, "雪")) {
    for (int i = -6; i <= 6; i += 6) {
      frame->drawFastHLine(x + i * s - 2 * s, y, 5 * s, COLOR_TEXT);
      frame->drawFastVLine(x + i * s, y - 2 * s, 5 * s, COLOR_TEXT);
    }
    return;
  }
  if (weatherContains(condition, "云") || weatherContains(condition, "阴")) {
    if (!weatherContains(condition, "阴")) {
      frame->fillCircle(x - 6 * s, y - 5 * s, 6 * s, COLOR_YELLOW);
    }
    frame->fillCircle(x - 4 * s, y, 7 * s, COLOR_MUTED);
    frame->fillCircle(x + 4 * s, y - 2 * s, 9 * s, COLOR_MUTED);
    frame->fillRect(x - 10 * s, y, 23 * s, 7 * s, COLOR_MUTED);
    return;
  }
  frame->fillCircle(x, y, 7 * s, COLOR_YELLOW);
  for (int angle = 0; angle < 8; ++angle) {
    const float radians = angle * 0.785398f;
    frame->drawLine(x + cosf(radians) * 10 * s, y + sinf(radians) * 10 * s,
                    x + cosf(radians) * 13 * s, y + sinf(radians) * 13 * s,
                    COLOR_YELLOW);
  }
}

void drawForecast(const Model &snapshot) {
  const bool compact = usingVoicePresentation(snapshot);
  if (portrait) {
    drawScrollingUtf8(textMargin, highResPanel ? 54 : 34,
                       screenWidth - 2 * textMargin, snapshot.city, COLOR_CYAN);
    if (snapshot.forecastCount == 0) {
      drawCenteredUtf8(screenHeight / 2, "天气预报获取中", COLOR_YELLOW);
      drawCenteredUtf8(screenHeight / 2 + textLineHeight, snapshot.weatherStatus, COLOR_MUTED);
      return;
    }
    if (highResPanel) drawUtf8(12, 80, "三天预报", COLOR_MUTED);
    for (size_t i = 0; i < snapshot.forecastCount && i < 3; ++i) {
      const int row = (highResPanel ? 96 : 42) + static_cast<int>(i) *
                      (highResPanel ? (compact ? 54 : 70) : (compact ? 27 : 37));
      const int x = highResPanel ? 78 : 36;
      const int textWidth = screenWidth - x - textMargin;
      drawWeatherIcon(snapshot.forecastCondition[i], highResPanel ? 38 : 17,
                       row + (highResPanel ? 24 : 12));
      drawFittedUtf8(x, row + (highResPanel ? (compact ? 9 : 12) : 10), textWidth, snapshot.forecastDay[i]);
      if (highResPanel) drawFittedUtf8(x, row + (compact ? 27 : 34), textWidth, snapshot.forecastCondition[i], COLOR_MUTED);
      char range[24];
      snprintf(range, sizeof(range), "%d~%dC", snapshot.forecastLowC[i], snapshot.forecastHighC[i]);
      drawFittedUtf8(x, row + (highResPanel ? (compact ? 45 : 56) : (compact ? 24 : 26)), textWidth, range);
    }
    return;
  }
  if (highResPanel) {
    drawScrollingUtf8(12, 54, 202, snapshot.city, COLOR_CYAN);
    drawFittedUtf8(231, 54, 80, "三天预报", COLOR_MUTED);
    if (snapshot.forecastCount == 0) {
      drawCenteredUtf8(116, "天气预报获取中", COLOR_YELLOW);
      drawCenteredUtf8(154, snapshot.weatherStatus, COLOR_MUTED);
      return;
    }
    const int columnWidth = screenWidth / 3;
    for (size_t i = 0; i < snapshot.forecastCount && i < 3; ++i) {
      const int center = static_cast<int>(i) * columnWidth + columnWidth / 2;
      drawCenteredRegion(center - columnWidth / 2 + 3, columnWidth - 6, 82,
                         snapshot.forecastDay[i], COLOR_TEXT);
      drawWeatherIcon(snapshot.forecastCondition[i], center, compact ? 112 : 120);
      drawCenteredRegion(center - columnWidth / 2 + 3, columnWidth - 6, compact ? 151 : 171,
                         snapshot.forecastCondition[i], COLOR_MUTED);
      char range[24];
      snprintf(range, sizeof(range), "%d~%dC", snapshot.forecastLowC[i], snapshot.forecastHighC[i]);
      drawCenteredRegion(center - columnWidth / 2 + 3, columnWidth - 6, compact ? 175 : 205, range, COLOR_TEXT);
    }
    return;
  }

  drawScrollingUtf8(5, 34, 90, snapshot.city, COLOR_CYAN);
  drawFittedUtf8(104, 34, 52, "3天预报", COLOR_MUTED);
  if (snapshot.forecastCount == 0) {
    drawCenteredUtf8(73, "天气预报获取中", COLOR_YELLOW);
    drawCenteredUtf8(96, snapshot.weatherStatus, COLOR_MUTED);
    return;
  }
  const int columnWidth = screenWidth / 3;
  for (size_t i = 0; i < snapshot.forecastCount && i < 3; ++i) {
    const int center = static_cast<int>(i) * columnWidth + columnWidth / 2;
    drawCenteredRegion(center - columnWidth / 2 + 3, columnWidth - 6, 51,
                       snapshot.forecastDay[i], COLOR_TEXT);
    drawWeatherIcon(snapshot.forecastCondition[i], center, 68);
    if (!compact) drawCenteredRegion(center - columnWidth / 2 + 3, columnWidth - 6, 96,
                                     snapshot.forecastCondition[i], COLOR_MUTED);
    char range[24];
    snprintf(range, sizeof(range), "%d~%dC", snapshot.forecastLowC[i], snapshot.forecastHighC[i]);
    drawCenteredRegion(center - columnWidth / 2 + 3, columnWidth - 6, compact ? 93 : 114, range, COLOR_TEXT);
  }
}

size_t memoTextPage(const String &text, size_t offset, int x, int baseline,
                    int width, int lines, bool paint) {
  char line[160];
  for (int row = 0; row < lines && offset < text.length(); ++row) {
    while (offset < text.length() && (text[offset] == ' ' || text[offset] == '\n' || text[offset] == '\r')) ++offset;
    if (offset >= text.length()) break;
    size_t end = fitTextLine(text, offset, width, line);
    // Keep closing punctuation with the preceding glyph when wrapping.
    if (end > offset && end + 3 <= text.length() && text[end - 1] != '\n' && text[end - 1] != '\r' &&
        (strncmp(text.c_str() + end, "。", 3) == 0 || strncmp(text.c_str() + end, "，", 3) == 0 ||
         strncmp(text.c_str() + end, "！", 3) == 0 || strncmp(text.c_str() + end, "？", 3) == 0 ||
         strncmp(text.c_str() + end, "；", 3) == 0 || strncmp(text.c_str() + end, "：", 3) == 0)) {
      const size_t used = strlen(line);
      size_t last = used;
      if (last > 0) --last;
      while (last > 0 && (static_cast<uint8_t>(line[last]) & 0xC0) == 0x80) --last;
      if (last > 0) { end -= used - last; line[last] = 0; }
    }
    if (paint) {
      textRenderer.setForegroundColor(COLOR_TEXT);
      textRenderer.drawUTF8(x, baseline + row * textLineHeight, line);
    }
    offset = end > offset ? end : nextUtf8Index(text, offset);
  }
  return offset;
}

void drawMemoPanel(int x, int y, int width, int height, const String &title,
                    size_t count, const String &text, uint16_t accent) {
  const int padding = highResPanel ? 10 : 5;
  const int fontHeight = highResPanel ? 16 : 12;
  const int textWidth = width - 2 * padding;
  const int headingBaseline = y + padding + fontHeight;
  const int firstBaseline = headingBaseline + textLineHeight + (highResPanel ? 7 : 4);
  const int footerBaseline = y + height - padding;
  const int lines = max(1, (footerBaseline - fontHeight - 4 - firstBaseline) / textLineHeight + 1);
  frame->fillRoundRect(x, y, width, height, highResPanel ? 8 : 5, COLOR_PANEL);
  frame->drawRoundRect(x, y, width, height, highResPanel ? 8 : 5, accent);
  drawFittedUtf8(x + padding, headingBaseline, textWidth, title + " " + String(count), accent);
  frame->drawFastHLine(x + padding, headingBaseline + 5, textWidth, COLOR_MUTED);
  size_t pages = 0, end = 0;
  do {
    end = memoTextPage(text, end, 0, 0, textWidth, lines, false);
    ++pages;
  } while (end < text.length());
  const size_t page = ((millis() - idlePageStartedAtMs) / TEXT_PAGE_INTERVAL_MS) % pages;
  size_t offset = 0;
  for (size_t i = 0; i < page; ++i) offset = memoTextPage(text, offset, 0, 0, textWidth, lines, false);
  frame->setClipRect(x + padding, firstBaseline - fontHeight, textWidth, lines * textLineHeight + fontHeight);
  memoTextPage(text, offset, x + padding, firstBaseline, textWidth, lines, true);
  frame->resetClip();
  if (pages > 1) {
    drawFittedUtf8(x + padding, footerBaseline, textWidth,
                   String(page + 1) + "/" + String(pages), COLOR_MUTED);
  }
}

void drawMemos(const Model &snapshot) {
  const int margin = highResPanel ? 10 : 5;
  const int gap = highResPanel ? 8 : 6;
  const int top = headerHeight + margin;
  const int bottom = screenHeight - (usingVoicePresentation(snapshot) ? (highResPanel ? 54 : 32) : 0);
  const int height = bottom - top - margin;
  const int availableWidth = screenWidth - 2 * margin;
  if (snapshot.noteCount && snapshot.todoCount) {
    const int leftWidth = (availableWidth - gap) / 2;
    drawMemoPanel(margin, top, leftWidth, height, "便签", snapshot.noteCount, snapshot.noteText, COLOR_CYAN);
    drawMemoPanel(margin + leftWidth + gap, top, availableWidth - leftWidth - gap, height,
                   "待办", snapshot.todoCount, snapshot.todoText, COLOR_GREEN);
  } else if (snapshot.noteCount || snapshot.todoCount) {
    const bool notes = snapshot.noteCount > 0;
    drawMemoPanel(margin, top, availableWidth, height, notes ? "便签" : "待办",
                   notes ? snapshot.noteCount : snapshot.todoCount,
                   notes ? snapshot.noteText : snapshot.todoText, notes ? COLOR_CYAN : COLOR_GREEN);
  } else {
    drawCenteredRegion(margin, availableWidth, top + height / 2, "暂无便签和待办", COLOR_MUTED);
  }
}

uint8_t idlePage(const Model &snapshot, uint32_t now) {
  if (snapshot.photoMode && RobotPhoto::available()) return PAGE_PHOTO;
  uint8_t pages[5];
  size_t pageCount = 0;
  if (snapshot.pageMask & PAGE_EXPRESSION) pages[pageCount++] = PAGE_EXPRESSION;
  if (snapshot.pageMask & PAGE_HOME) pages[pageCount++] = PAGE_HOME;
  if (snapshot.pageMask & PAGE_FORECAST) pages[pageCount++] = PAGE_FORECAST;
  if (snapshot.pageMask & PAGE_MEMOS) pages[pageCount++] = PAGE_MEMOS;
  if (snapshot.pageMask & PAGE_PHOTO) pages[pageCount++] = PAGE_PHOTO;
  if (pageCount == 0) pages[pageCount++] = PAGE_HOME;
  if (lastFrameScene != Scene::Idle || lastIdleMask != snapshot.pageMask) {
    idleStartedAtMs = now;
    idlePageStartedAtMs = now;
    lastIdleMask = snapshot.pageMask;
  }
  const uint8_t page = pages[((now - idleStartedAtMs) / IDLE_CAROUSEL_INTERVAL_MS) % pageCount];
  if (page != lastIdlePage) idlePageStartedAtMs = now;
  return page;
}

void drawIdle(const Model &snapshot, uint8_t page) {
  if (page == PAGE_EXPRESSION) {
    drawExpression(snapshot);
    return;
  }
  if (page == PAGE_FORECAST) {
    drawForecast(snapshot);
    return;
  }
  if (page == PAGE_MEMOS) {
    drawMemos(snapshot);
    return;
  }

  const bool compact = usingVoicePresentation(snapshot);
  if (!compact) textPageOffset = 0;
  // Compact voice state belongs to the bottom strip; the home avatar stays idle.
  const Scene avatarScene = compact ? Scene::Idle : snapshot.scene;
  char date[24];
  localClock(date, sizeof(date), "%m-%d %a");
  char temperature[24];
  snprintf(temperature, sizeof(temperature), "%.1f C", snapshot.temperatureC);
  if (!portrait || !portraitBodyEnabled()) {
    const int bottom = screenHeight - (compact ? (highResPanel ? 54 : 32) : 0);
    drawAmbientBackground(bottom);
    if (highResPanel) {
      const int clockBaseline = portrait ? 80 : (compact ? 61 : 73);
      drawClock(portrait ? -1 : 16, clockBaseline);
      if (portrait) drawCenteredUtf8(103, date, COLOR_MUTED);
      else drawFittedUtf8(164, clockBaseline - 3, screenWidth - 180, date, COLOR_MUTED);
      const int faceTop = portrait ? 115 : (compact ? 65 : 84);
      const int faceHeight = portrait ? (compact ? 86 : 110) : (compact ? 75 : 100);
      drawAvatar(avatarScene, portrait ? 14 : 40, faceTop,
                 screenWidth - (portrait ? 28 : 80), faceHeight, false);
      const int weatherTop = portrait ? (compact ? 203 : 238) : (compact ? 142 : 184);
      const int iconX = portrait ? 37 : 34;
      if (snapshot.weatherValid) drawWeatherIcon(snapshot.weatherCondition, iconX, weatherTop + 23);
      const int tx = portrait ? 67 : 65;
      drawScrollingUtf8(tx, weatherTop + 17, screenWidth - tx - 12, snapshot.city, COLOR_TEXT);
      const String description = snapshot.weatherValid ? snapshot.weatherCondition + "  " + temperature : snapshot.weatherStatus;
      drawFittedUtf8(tx, weatherTop + 40, screenWidth - tx - 12, description, COLOR_TEXT);
      if (portrait && !compact && !snapshot.alarmStatus.isEmpty())
        drawScrollingUtf8(12, 313, 216, snapshot.alarmStatus, COLOR_MUTED);
    } else {
      drawClock(portrait ? -1 : 6, 44);
      if (portrait) drawCenteredUtf8(60, date, COLOR_MUTED);
      else drawFittedUtf8(83, 40, 72, date, COLOR_MUTED);
      const int faceTop = portrait ? 65 : 46;
      const int faceHeight = portrait ? (compact ? 34 : 51) : (compact ? 31 : 49);
      drawAvatar(avatarScene, 10, faceTop, screenWidth - 20, faceHeight, false);
      const String description = snapshot.weatherValid ? snapshot.weatherCondition + " " + temperature : snapshot.weatherStatus;
      if (!compact) drawScrollingUtf8(8, bottom - 21, screenWidth - 16, snapshot.city, COLOR_TEXT);
      drawFittedUtf8(8, bottom - 5, screenWidth - 16, description, COLOR_TEXT);
    }
    return;
  }
  if (portrait) {
    if (highResPanel) {
      drawAvatar(avatarScene, 60, 33, 120, compact ? 94 : 108, true);
      drawClock(-1, compact ? 163 : 177);
      drawCenteredUtf8(compact ? 188 : 202, date, COLOR_MUTED);
      drawScrollingUtf8(12, compact ? 211 : 229, 216, snapshot.city, COLOR_CYAN);
      drawCenteredUtf8(compact ? 234 : 254, snapshot.weatherValid ? snapshot.weatherCondition : snapshot.weatherStatus,
                       snapshot.weatherValid ? COLOR_TEXT : COLOR_YELLOW);
      if (snapshot.weatherValid) drawCenteredUtf8(compact ? 257 : 278, temperature);
      frame->drawFastHLine(12, 287, 216, COLOR_PANEL);
      drawUtf8(12, 310, "闹钟", COLOR_CYAN);
      drawScrollingUtf8(52, 310, 176, snapshot.alarmStatus, COLOR_MUTED);
    } else {
      drawAvatar(avatarScene, 4, 23, 43, compact ? 55 : 67, true);
      drawClock(50, 48);
      drawFittedUtf8(50, 65, 73, date, COLOR_MUTED);
      drawScrollingUtf8(5, compact ? 90 : 101, 118, snapshot.city, COLOR_CYAN);
      drawFittedUtf8(5, compact ? 106 : 117, 118, snapshot.weatherValid ? snapshot.weatherCondition : snapshot.weatherStatus,
                     snapshot.weatherValid ? COLOR_TEXT : COLOR_YELLOW);
      if (snapshot.weatherValid) drawUtf8(5, compact ? 122 : 133, temperature);
      frame->drawFastHLine(5, 137, 118, COLOR_PANEL);
      drawScrollingUtf8(5, 151, 118, snapshot.alarmStatus, COLOR_MUTED);
    }
    return;
  }
}

void drawSetup(const Model &snapshot) {
  prepareSetupQr(snapshot);
  const bool openPage = snapshot.phoneConnected;
  const SetupQr &qr = openPage ? urlQr : wifiQr;
  const String heading = openPage ? "2 打开配置页" : "1 连接热点";
  String host = snapshot.setupUrl;
  host.replace("http://", "");
  const int slash = host.indexOf('/');
  if (slash >= 0) host = host.substring(0, slash);
  const int moduleCount = qr.size ? qr.size + 8 : 37;  // Four white modules on every edge.
  const int available = portrait
      ? min(screenWidth - 12, screenHeight - headerHeight - (highResPanel ? 110 : 63))
      : min(screenHeight - headerHeight - 12, screenWidth * 3 / 5 - 12);
  const int scale = max(1, available / moduleCount);
  const int side = moduleCount * scale;
  const int left = portrait ? (screenWidth - side) / 2 : (highResPanel ? 6 : 4);
  const int top = portrait ? headerHeight + (highResPanel ? 32 : 19)
                           : headerHeight + (screenHeight - headerHeight - side) / 2;
  if (qr.size) {
    frame->fillRect(left, top, side, side, ST77XX_WHITE);
    for (int y = 0; y < qr.size; ++y) for (int x = 0; x < qr.size; ++x) {
      if (qr.black(x, y)) frame->fillRect(left + (x + 4) * scale, top + (y + 4) * scale,
                                         scale, scale, ST77XX_BLACK);
    }
  } else {
    drawCenteredRegion(left, side, top + side / 2, "请手动配置", COLOR_YELLOW);
  }
  if (portrait) {
    drawCenteredUtf8(headerHeight + (highResPanel ? 22 : 14), heading, COLOR_CYAN);
    const int baseline = top + side + (highResPanel ? 24 : 13);
    drawCenteredUtf8(baseline, openPage ? host : snapshot.setupSsid);
    drawCenteredUtf8(baseline + textLineHeight,
                     openPage ? String("/setup") : String("密码 ") + snapshot.setupPassword);
    drawCenteredUtf8(baseline + 2 * textLineHeight,
                     openPage ? "连接码登录" : "连上后再扫码", COLOR_MUTED);
  } else {
    const int tx = left + side + (highResPanel ? 10 : 6);
    const int tw = screenWidth - tx - 3;
    const int baseline = headerHeight + (highResPanel ? 27 : 17);
    drawFittedUtf8(tx, baseline, tw, heading, COLOR_CYAN);
    // ASCII SSID/password/IP fit even alongside the QR on a 160-pixel screen.
    setTransparentFont(highResPanel ? u8g2_font_7x14_tr : u8g2_font_6x10_tr);
    drawFittedUtf8(tx, baseline + textLineHeight + 2, tw, openPage ? host : snapshot.setupSsid);
    drawFittedUtf8(tx, baseline + 2 * textLineHeight + 2, tw,
                   openPage ? String("/setup") : snapshot.setupPassword);
    restoreBodyFont();
    drawFittedUtf8(tx, baseline + 3 * textLineHeight + 4, tw,
                   openPage ? "连接码登录" : "热点密码", COLOR_MUTED);
    drawFittedUtf8(tx, baseline + 4 * textLineHeight + 4, tw,
                   openPage ? "先保持联网" : "连上再扫码", COLOR_MUTED);
  }
}

void drawVoiceStrip(const Model &snapshot) {
  const int height = highResPanel ? 54 : 32;
  const int top = screenHeight - height;
  frame->fillRect(0, top, screenWidth, height, COLOR_PANEL);
  frame->drawFastHLine(0, top, screenWidth, sceneColor(snapshot.scene));
  const int iconWidth = highResPanel ? 58 : 30;
  // The small status eyes have their own background, matching the strip.
  frame->fillRoundRect(3, top + 3, iconWidth, height - 6, 3, COLOR_BG);
  drawAvatar(snapshot.scene, 3, top + 3, iconWidth, height - 6, false);
  const int tx = iconWidth + (highResPanel ? 12 : 7);
  drawFittedUtf8(tx, top + (highResPanel ? 21 : 13), screenWidth - tx - 4,
                 snapshot.title, sceneColor(snapshot.scene));
  const String &content = snapshot.body.isEmpty() ? snapshot.question : snapshot.body;
  char line[160];
  const size_t offset = textPageOffset < content.length() ? textPageOffset : 0;
  const size_t end = fitTextLine(content, offset, screenWidth - tx - 4, line);
  textRenderer.setForegroundColor(COLOR_TEXT);
  textRenderer.drawUTF8(tx, screenHeight - (highResPanel ? 10 : 4), line);
  nextTextPageOffset = end >= content.length() ? 0 :
                      end > offset ? end : nextUtf8Index(content, offset);
}

void drawFullStatus(const Model &snapshot) {
  const int titleY = headerHeight + (highResPanel ? 24 : 16);
  drawCenteredUtf8(titleY, snapshot.title, sceneColor(snapshot.scene));
  const String &content = snapshot.body.isEmpty() ? snapshot.question : snapshot.body;
  const int avatarY = titleY + (highResPanel ? 8 : 4);
  const int avatarHeight = content.isEmpty() ? screenHeight - avatarY - 8 :
                           portrait ? (highResPanel ? 150 : 62) : (highResPanel ? 58 : 24);
  drawAvatar(snapshot.scene, textMargin, avatarY, screenWidth - 2 * textMargin,
             avatarHeight, portrait && portraitBodyEnabled());
  const int textY = avatarY + avatarHeight + textLineHeight;
  nextTextPageOffset = drawTextPage(content, textPageOffset, textY,
      max(0, (screenHeight - 5 - textY) / textLineHeight + 1));
}

void render(const Model &snapshot) {
  const uint32_t now = millis();
  const bool preview = snapshot.scene == Scene::Idle && !snapshot.provisioning &&
                       snapshot.previewUntilMs != 0 && static_cast<int32_t>(snapshot.previewUntilMs - now) > 0;
  const bool dialogueMood = snapshot.voiceActive && snapshot.hasDialogueExpression &&
      (snapshot.expressions & (uint64_t{1} << snapshot.dialogueExpression));
  frameExpression = preview ? snapshot.previewId : dialogueMood ? snapshot.dialogueExpression :
      idleExpression(now, snapshot.expressions);
  static bool wasPreview = false;
  static uint32_t previewStartedAtMs = 0;
  static uint8_t previewPreviousPage = PAGE_HOME;
  if (preview && !wasPreview) {
    previewStartedAtMs = now;
    previewPreviousPage = lastIdlePage;
  } else if (!preview && wasPreview) {
    // Freeze the carousel clock during a preview, including manual cancellation.
    const uint32_t pausedMs = now - previewStartedAtMs;
    idleStartedAtMs += pausedMs;
    idlePageStartedAtMs += pausedMs;
    lastIdlePage = previewPreviousPage;
  }
  wasPreview = preview;
  const bool voice = usingVoicePresentation(snapshot);
  const bool setupVisible = snapshot.provisioning && !isVoiceScene(snapshot.scene) &&
                            snapshot.scene != Scene::Alarm && snapshot.scene != Scene::Updating;
  const bool compactVoice = voice && !voiceFullscreen(snapshot.voiceFromWake) && !setupVisible;
  static uint8_t voicePage = PAGE_HOME;
  static bool lastCompactVoice = false;
  static bool lastVoice = false;
  if (voice && !lastVoice) voicePage = lastIdlePage ? lastIdlePage : PAGE_HOME;
  const bool focusPage = snapshot.focusActive && snapshot.scene == Scene::Idle && !preview && !setupVisible;
  const uint8_t page = focusPage ? 0 : setupVisible ? 0 : preview ? PAGE_EXPRESSION : compactVoice ? voicePage :
                       snapshot.scene == Scene::Idle ? idlePage(snapshot, now) : 0;
  const bool photoPage = page == PAGE_PHOTO;
  const uint32_t photoRevision = photoPage ? RobotPhoto::revision() : 0;
  const bool keepPhoto = photoPage && (lastFrameScene == Scene::Idle || lastVoice) &&
                         lastIdlePage == PAGE_PHOTO && cachedPhotoRevision == photoRevision &&
                         !(lastCompactVoice && !compactVoice);
  if (!keepPhoto) {
    frame->fillScreen(COLOR_BG);
    photoPainted = false;
    lastPhotoAttemptAtMs = 0;
  }
  if (setupVisible) {
    drawSetup(snapshot);
  } else if (focusPage) {
    drawCenteredUtf8(headerHeight + textLineHeight + 8, "番茄钟", COLOR_CYAN);
    nextTextPageOffset = drawTextPage(snapshot.focusStatus, 0,
        headerHeight + textLineHeight * 3, max(1, (screenHeight - headerHeight) / textLineHeight - 4));
  } else if (photoPage) {
    // Retain the full-screen photo in the existing PSRAM framebuffer.
    // Only a new page/file (or a bounded retry) reads flash again.
    if (!photoPainted && (!keepPhoto || now - lastPhotoAttemptAtMs >= 1000U)) {
      frame->fillScreen(COLOR_BG);
      const bool available = RobotPhoto::available();
      photoPainted = available && RobotPhoto::copyTo(frame->getBuffer(), screenWidth,
                                                    screenHeight);
      if (!photoPainted) {
        // copyTo may have written some rows before an I/O failure.
        frame->fillScreen(COLOR_BG);
        drawCenteredUtf8(screenHeight / 2 - 5,
                         available ? "照片暂不可用" : "尚未上传照片", COLOR_MUTED);
        drawCenteredUtf8(screenHeight / 2 + textLineHeight, "在管理网页上传图片", COLOR_CYAN);
      }
      lastPhotoAttemptAtMs = now;
    }
    cachedPhotoRevision = photoRevision;
  } else if (snapshot.scene == Scene::Idle || compactVoice) {
    drawIdle(snapshot, page);
  } else {
    drawFullStatus(snapshot);
  }
  if (compactVoice) drawVoiceStrip(snapshot);
  // A displayed photo owns every pixel, including the top strip. Keep the IP
  // on all other pages and on the missing/unavailable-photo placeholder.
  if (!photoPage || !photoPainted) drawHeader(snapshot);
  lastIdlePage = page;
  lastFrameScene = snapshot.scene;
  lastCompactVoice = compactVoice;
  lastVoice = voice;
  updateBrightness(setupVisible);
  if (!tft) return;

  // The high-resolution framebuffer lives in PSRAM. Copy one scan line at a
  // time to a small internal-RAM buffer before handing it to SPI; this avoids
  // tying the audio path to large DMA-capable internal allocations.
  const uint16_t *pixels = frame->getBuffer();
  const uint8_t dim = backlightPwm ? 100 : appliedBrightness.load();
  static uint8_t tablePercent = 0;
  static uint16_t red[32], green[64], blue[32];
  if (dim != tablePercent) {
    for (int i = 0; i < 32; ++i) {
      blue[i] = (i * dim + 50) / 100;
      red[i] = blue[i] << 11;
    }
    for (int i = 0; i < 64; ++i) green[i] = ((i * dim + 50) / 100) << 5;
    tablePercent = dim;
  }
  tft->startWrite();
  tft->setAddrWindow(0, 0, screenWidth, screenHeight);
  for (int y = 0; y < screenHeight; ++y) {
    memcpy(scanline, pixels + static_cast<size_t>(y) * screenWidth,
           screenWidth * sizeof(uint16_t));
    if (dim < 100) for (int x = 0; x < screenWidth; ++x) {
      const uint16_t pixel = scanline[x];
      scanline[x] = red[pixel >> 11] | green[(pixel >> 5) & 63] | blue[pixel & 31];
    }
    tft->writePixels(scanline, screenWidth, true);
  }
  tft->endWrite();
}

void displayTask(void *) {
  uint32_t lastFrameAtMs = 0;
  // Keep one snapshot alive for the lifetime of the task. Reconstructing a
  // Model every 20 ms repeatedly allocates/frees Arduino String buffers and
  // fragments the same internal heap later needed by audio/TLS.
  Model snapshot;
  for (;;) {
    const uint32_t now = millis();
    if (now - lastBatterySampleAtMs >= BATTERY_SAMPLE_INTERVAL_MS || lastBatterySampleAtMs == 0) {
      sampleBattery();
      lastBatterySampleAtMs = now;
    }

    if (xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
      if (model.previewUntilMs && static_cast<int32_t>(now - model.previewUntilMs) >= 0) {
        model.previewUntilMs = 0;
        ++model.revision;
      }
      if (snapshot.revision != model.revision ||
          snapshot.networkConnected != model.networkConnected ||
          snapshot.rssi != model.rssi || strcmp(snapshot.ip, model.ip) != 0) {
        snapshot = model;
      }
      xSemaphoreGive(modelMutex);
    }
    if (snapshot.scene != renderedScene || snapshot.revision != renderedRevision) {
      textPageOffset = 0;
      nextTextPageOffset = 0;
      renderedScene = snapshot.scene;
      renderedRevision = snapshot.revision;
      lastTextPageAtMs = now;
    } else if (now - lastTextPageAtMs >= TEXT_PAGE_INTERVAL_MS) {
      textPageOffset = nextTextPageOffset;
      lastTextPageAtMs = now;
    }
    const uint32_t frameInterval = snapshot.scene == Scene::Idle
                                       ? IDLE_FRAME_INTERVAL_MS
                                       : (highResPanel
                                              ? (snapshot.scene == Scene::Listening ? 300U : 160U)
                                              : ACTIVE_FRAME_INTERVAL_MS);
    if (!suspended && (now - lastFrameAtMs >= frameInterval || lastFrameAtMs == 0) &&
        xSemaphoreTake(ioMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (!suspended) {
        render(snapshot);
        lastFrameAtMs = now;
      }
      xSemaphoreGive(ioMutex);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

}  // namespace

Panel currentPanel() {
  return activePanel;
}

uint8_t currentRotation() { return activeRotation; }
uint16_t width() { return screenWidth; }
uint16_t height() { return screenHeight; }
uint16_t contentTop() { return headerHeight; }

bool saveRotationPreference(uint8_t rotation) {
  if (rotation > 3) return false;
  Preferences storage;
  if (!storage.begin(DISPLAY_PREF_NAMESPACE, false)) return false;
  const bool ok = storage.putUChar(ROTATION_PREF_KEY, rotation) > 0;
  storage.end();
  return ok;
}

const char *panelId(Panel panel) {
  return panel == Panel::ST7735S_128X160_8PIN ? "st7735" : "st7789";
}

const char *panelLabel(Panel panel) {
  return panel == Panel::ST7735S_128X160_8PIN
             ? "ST7735S 8P 128x160"
             : "M200T ST7789 7P 240x320";
}

bool panelFromId(const String &id, Panel &panel) {
  String normalized = id;
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "st7735" || normalized == "7735" || normalized == "8p") {
    panel = Panel::ST7735S_128X160_8PIN;
    return true;
  }
  if (normalized == "st7789" || normalized == "7789" || normalized == "7p" ||
      normalized == "m200t") {
    panel = Panel::M200T_ST7789_240X320_7PIN;
    return true;
  }
  return false;
}

bool savePanelPreference(Panel panel) {
  Preferences storage;
  if (!storage.begin(DISPLAY_PREF_NAMESPACE, false)) return false;
  const bool ok = storage.putString(DISPLAY_PREF_KEY, panelId(panel)) > 0;
  storage.end();
  return ok;
}

uint8_t brightnessSetting() { return brightnessConfig.load() & 0xFF; }
uint8_t effectiveBrightness() { return appliedBrightness.load(); }
bool potentiometerBrightnessEnabled() { return (brightnessConfig.load() & BRIGHTNESS_POT_FLAG) != 0; }
bool hardwareBacklightControl() { return backlightPwm.load(); }

bool saveBrightnessPreference(uint8_t percent, bool potentiometer) {
  if (percent < MIN_BRIGHTNESS || percent > 100 ||
      (potentiometer && BATTERY_MONITOR_ENABLED)) return false;
  Preferences storage;
  if (!storage.begin(DISPLAY_PREF_NAMESPACE, false)) return false;
  if (potentiometer && storage.getBool("volume_pot", false)) { storage.end(); return false; }
  const uint16_t value = percent | (potentiometer ? BRIGHTNESS_POT_FLAG : 0);
  const bool ok = storage.putUShort(BRIGHTNESS_PREF_KEY, value) > 0;
  storage.end();
  if (ok) brightnessConfig.store(value);
  return ok;
}

bool voiceFullscreen(bool wakeTriggered) {
  return (voiceLayoutConfig.load() & (wakeTriggered ? 2U : 1U)) != 0;
}

bool portraitBodyEnabled() { return portraitBodyConfig.load(); }

bool savePortraitBodyPreference(bool enabled) {
  Preferences storage;
  if (!storage.begin(DISPLAY_PREF_NAMESPACE, false)) return false;
  const bool ok = storage.putBool(BODY_PREF_KEY, enabled) > 0;
  storage.end();
  if (ok) portraitBodyConfig.store(enabled);
  return ok;
}

bool saveVoiceLayoutPreferences(bool buttonFullscreen, bool wakeFullscreen) {
  const uint8_t value = (buttonFullscreen ? 1 : 0) | (wakeFullscreen ? 2 : 0);
  Preferences storage;
  if (!storage.begin(DISPLAY_PREF_NAMESPACE, false)) return false;
  const bool ok = storage.putUChar(VOICE_LAYOUT_PREF_KEY, value) > 0;
  storage.end();
  if (ok) {
    voiceLayoutConfig.store(value);
    if (modelMutex && xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
      ++model.revision;
      xSemaphoreGive(modelMutex);
    }
  }
  return ok;
}

void setVoiceSource(bool wakeTriggered) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.voiceFromWake = wakeTriggered;
  model.voiceActive = true;
  model.previewUntilMs = 0;
  ++model.revision;
  xSemaphoreGive(modelMutex);
}

uint64_t expressionMask() {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return 0;
  const uint64_t mask = model.expressions;
  xSemaphoreGive(modelMutex);
  return mask;
}

bool saveExpressionMask(uint64_t mask) {
  if (!RobotExpressions::validMask(mask) || !modelMutex ||
      xSemaphoreTake(modelMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  Preferences storage;
  bool ok = storage.begin(DISPLAY_PREF_NAMESPACE, false);
  if (ok) { ok = storage.putULong64(EXPRESSIONS_PREF_KEY, mask) > 0; storage.end(); }
  if (ok) { model.expressions = mask; model.previewUntilMs = 0; ++model.revision; }
  xSemaphoreGive(modelMutex);
  return ok;
}

bool previewExpression(uint8_t id) {
  if (id >= RobotExpressions::COUNT || !modelMutex ||
      xSemaphoreTake(modelMutex, pdMS_TO_TICKS(100)) != pdTRUE) return false;
  const bool allowed = model.scene == Scene::Idle && !model.provisioning && !model.voiceActive;
  if (allowed) {
    model.previewId = id;
    model.previewUntilMs = millis() + 8000U;
    if (!model.previewUntilMs) model.previewUntilMs = 1;
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
  return allowed;
}

void cancelExpressionPreview() {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.previewUntilMs = 0;
  ++model.revision;
  xSemaphoreGive(modelMutex);
}

void setProvisioning(bool active, const String &ssid, const String &password,
                     const String &url, bool phoneConnected) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.provisioning != active || model.setupSsid != ssid ||
      model.setupPassword != password || model.setupUrl != url ||
      model.phoneConnected != phoneConnected) {
    model.provisioning = active;
    if (active) model.previewUntilMs = 0;
    model.setupSsid = ssid;
    model.setupPassword = password;
    model.setupUrl = url;
    model.phoneConnected = phoneConnected;
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

bool begin() {
  if (initialized) return true;
  const Panel selectedPanel = loadPanelPreference();
  configurePanelGeometry(selectedPanel, loadRotationPreference(selectedPanel));
  Preferences storage;
  if (storage.begin(DISPLAY_PREF_NAMESPACE, true)) {
    uint16_t value = storage.getUShort(BRIGHTNESS_PREF_KEY, 100);
    if ((value & 0xFF) < MIN_BRIGHTNESS || (value & 0xFF) > 100) value = 100;
    value &= 0x1FF;
    if (BATTERY_MONITOR_ENABLED || storage.getBool("volume_pot", false)) value &= 0xFF;
    brightnessConfig.store(value);
    voiceLayoutConfig.store(storage.getUChar(VOICE_LAYOUT_PREF_KEY, 0) & 3);
    portraitBodyConfig.store(storage.getBool(BODY_PREF_KEY, false));
    const uint64_t savedExpressions = storage.getULong64(EXPRESSIONS_PREF_KEY, RobotExpressions::ALL);
    model.expressions = RobotExpressions::validMask(savedExpressions) ? savedExpressions : RobotExpressions::ALL;
    storage.end();
  }
  Serial.printf("[显示] NVS 屏型=%s（%s）。\n", panelId(activePanel), panelLabel(activePanel));
  // Only the 8-pin ST7735S exposes a separate BLK input. The 7-pin M200T
  // ST7789 module has no BLK pin, so GPIO16 is deliberately left untouched.
  if (activePanel == Panel::ST7735S_128X160_8PIN && PIN_TFT_BACKLIGHT >= 0) {
    pinMode(PIN_TFT_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_TFT_BACKLIGHT, HIGH);
    // Arduino-ESP32 3.3.11 allocates a separate timer for 5 kHz / 8-bit PWM;
    // the existing four SG90 outputs keep their shared 50 Hz timer.
    backlightPwm = ledcAttach(PIN_TFT_BACKLIGHT, 5000, 8);
    Serial.printf("[显示] ST7735S 背光 GPIO%d，调节方式=%s。\n",
                  PIN_TFT_BACKLIGHT, backlightPwm ? "PWM" : "软件压暗");
  }
  modelMutex = xSemaphoreCreateMutex();
  ioMutex = xSemaphoreCreateMutex();
  if (!modelMutex || !ioMutex) {
    if (modelMutex) vSemaphoreDelete(modelMutex);
    if (ioMutex) vSemaphoreDelete(ioMutex);
    modelMutex = nullptr;
    ioMutex = nullptr;
    return false;
  }
  // 320x240x16-bit is 150 KiB. On ST7789 this buffer must live in PSRAM so
  // microphone/I2S/TLS work keeps the internal heap and its largest free block.
  frame = new (std::nothrow) DisplayCanvas16(screenWidth, screenHeight, highResPanel);
  if (!frame || !frame->getBuffer()) {
    delete frame;
    frame = nullptr;
    vSemaphoreDelete(modelMutex);
    vSemaphoreDelete(ioMutex);
    modelMutex = nullptr;
    ioMutex = nullptr;
    Serial.printf("[显示] 申请 %u KB 帧缓冲失败%s。\n",
                  static_cast<unsigned>((screenWidth * screenHeight * 2UL) / 1024UL),
                  highResPanel ? "（ST7789 要求 PSRAM）" : "");
    return false;
  }
  Serial.printf("[显示] 帧缓冲 %u KB，位置=%s；heap=%u，largest=%u。\n",
                static_cast<unsigned>((screenWidth * screenHeight * 2UL) / 1024UL),
                frame->inPsram() ? "PSRAM" : "内部RAM",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
  if (BATTERY_MONITOR_ENABLED) {
    pinMode(PIN_BATTERY_ADC, INPUT);
    analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  }
  displaySpi.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  if (activePanel == Panel::M200T_ST7789_240X320_7PIN) {
    tft = &tft7789;
    // The driver and framebuffer share the selected native address space.
    tft7789.init(240, 320);
    tft7789.setRotation(activeRotation);
  } else {
    tft = &tft7735;
    tft7735.initR(INITR_BLACKTAB);
    tft7735.setRotation(activeRotation);
  }
  Serial.printf("[显示] 旋转=%u 度，原生分辨率=%dx%d。\n",
                activeRotation * 90U, screenWidth, screenHeight);

  // Refuse to stream a framebuffer whose geometry does not match the panel.
  // This catches an incorrect init/rotation instead of producing wraparound,
  // flower-screen artifacts or stale strips at an edge.
  if (!tft || tft->width() != screenWidth || tft->height() != screenHeight) {
    Serial.printf("[显示] 分辨率异常：驱动=%dx%d，期望=%dx%d。\n",
                  tft ? tft->width() : 0, tft ? tft->height() : 0,
                  screenWidth, screenHeight);
    delete frame;
    frame = nullptr;
    vSemaphoreDelete(modelMutex);
    vSemaphoreDelete(ioMutex);
    modelMutex = nullptr;
    ioMutex = nullptr;
    tft = nullptr;
    return false;
  }

  // Establish the same background both on the physical GRAM and framebuffer.
  // Text pages clear to this background before drawing transparent glyphs.
  // Photo pages preserve their full-screen pixels between file/page changes.
  tft->setTextWrap(false);
  tft->fillScreen(COLOR_BG);
  frame->fillScreen(COLOR_BG);
  frame->setTextWrap(false);
  frame->setTextColor(COLOR_TEXT);  // one-argument form = transparent text

  textRenderer.begin(*frame);
  textRenderer.setFontMode(1);      // transparent glyph background
  textRenderer.setBackgroundColor(COLOR_BG);
  textRenderer.setForegroundColor(COLOR_TEXT);
  textRenderer.setFontDirection(0);
  restoreBodyFont();
  initialized = true;
  if (xTaskCreatePinnedToCore(displayTask, "robot-display", 6144, nullptr, 1,
                              &displayTaskHandle, ARDUINO_RUNNING_CORE) != pdPASS) {
    initialized = false;
    delete frame;
    frame = nullptr;
    vSemaphoreDelete(modelMutex);
    vSemaphoreDelete(ioMutex);
    modelMutex = nullptr;
    ioMutex = nullptr;
    return false;
  }
  return true;
}

bool ready() {
  return initialized;
}

void show(Scene scene, const String &title, const String &body) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.scene != scene || model.title != title || model.body != body) {
    model.scene = scene;
    model.title = title;
    model.body = body;
    if (scene != Scene::Idle) model.previewUntilMs = 0;
    if (scene == Scene::Idle || scene == Scene::Booting || scene == Scene::Alarm || scene == Scene::Updating) {
      model.voiceActive = false;
    }
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

void setConversation(const String &question, const String &answer) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.question != question || model.answer != answer) {
    model.question = question;
    model.answer = answer;
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

void clearConversation() {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (!model.question.isEmpty() || !model.answer.isEmpty()) {
    model.question = "";
    model.answer = "";
    ++model.revision;
  }
  model.hasDialogueExpression = false;
  xSemaphoreGive(modelMutex);
}

void setDialogueExpression(uint8_t id) {
  if (id >= RobotExpressions::COUNT || !modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.dialogueExpression = id; model.hasDialogueExpression = true; ++model.revision;
  xSemaphoreGive(modelMutex);
}

void setFocusStatus(bool active, const String &status) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.focusActive != active || model.focusStatus != status) {
    model.focusActive = active; model.focusStatus = status; ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

void setWeather(const String &city, float temperatureC, const String &condition, bool valid) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.city = city;
  model.temperatureC = temperatureC;
  model.weatherCondition = condition;
  model.weatherValid = valid;
  model.weatherStatus = valid ? "天气已更新" : "天气不可用";
  ++model.revision;
  xSemaphoreGive(modelMutex);
}

void setForecast(const String dayLabels[3], const String conditions[3],
                 const int highC[3], const int lowC[3], size_t count) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.forecastCount = min(count, static_cast<size_t>(3));
  for (size_t i = 0; i < 3; ++i) {
    model.forecastDay[i] = i < model.forecastCount ? dayLabels[i] : "";
    model.forecastCondition[i] = i < model.forecastCount ? conditions[i] : "";
    model.forecastHighC[i] = i < model.forecastCount ? highC[i] : 0;
    model.forecastLowC[i] = i < model.forecastCount ? lowC[i] : 0;
  }
  ++model.revision;
  xSemaphoreGive(modelMutex);
}

void setWeatherStatus(const String &city, const String &status) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.city = city;
  model.weatherStatus = status;
  ++model.revision;
  xSemaphoreGive(modelMutex);
}

void setAlarmStatus(const String &status) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.alarmStatus != status) {
    model.alarmStatus = status;
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

void setMemoStatus(const String &notes, const String &todos, size_t noteCount,
                   size_t todoCount, size_t pendingTodoCount) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.noteText != notes || model.todoText != todos || model.noteCount != noteCount || model.todoCount != todoCount ||
      model.pendingTodoCount != pendingTodoCount) {
    model.noteText = notes;
    model.todoText = todos;
    model.noteCount = noteCount;
    model.todoCount = todoCount;
    model.pendingTodoCount = pendingTodoCount;
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

void setPageMask(uint8_t pageMask) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  pageMask &= PAGE_ALL;
  if (pageMask == 0) pageMask = PAGE_HOME;
  if (model.pageMask != pageMask) {
    model.pageMask = pageMask;
    ++model.revision;
  }
  xSemaphoreGive(modelMutex);
}

void setPhotoMode(bool enabled) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  if (model.photoMode != enabled) { model.photoMode = enabled; ++model.revision; }
  xSemaphoreGive(modelMutex);
}

void setNetwork(bool connected, int rssi, const String &ip) {
  if (!modelMutex || xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;
  model.networkConnected = connected;
  model.rssi = rssi;
  ip.toCharArray(model.ip, sizeof(model.ip));
  xSemaphoreGive(modelMutex);
}

void setSuspended(bool value) {
  if (!initialized || !ioMutex) return;
  suspended = value;
  if (value) {
    // Wait until a transfer already in progress has completed. New frames
    // re-check suspended after taking the same mutex.
    if (xSemaphoreTake(ioMutex, pdMS_TO_TICKS(500)) == pdTRUE) xSemaphoreGive(ioMutex);
  } else if (modelMutex && xSemaphoreTake(modelMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
    ++model.revision;
    xSemaphoreGive(modelMutex);
  }
}

float batteryVoltage() {
  return measuredBatteryVoltage;
}

int batteryPercent() {
  return measuredBatteryPercent;
}

}  // namespace RobotDisplay
