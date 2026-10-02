#pragma once

#include <Arduino.h>

#include "hardware_pins.h"

namespace RobotDisplay {

// Supported SPI TFT modules. Both use the same SCLK/MOSI/RST/DC/CS wiring.
// The 8-pin ST7735S also has a separate BLK pin; the 7-pin M200T-ST7789
// module has no BLK pin and its backlight is powered internally from VCC.
enum class Panel : uint8_t {
  ST7735S_128X160_8PIN,
  M200T_ST7789_240X320_7PIN,
};

// Default used only when NVS has no saved panel selection yet.
constexpr Panel DEFAULT_PANEL = Panel::M200T_ST7789_240X320_7PIN;

// Driver rotations: 0/2 portrait, 1/3 landscape; saved selection overrides these.
constexpr uint8_t ST7789_ROTATION = 1;
constexpr uint8_t ST7735_ROTATION = 1;

// Hardware-SPI wiring. The display is powered from ESP32 3V3.
constexpr int PIN_TFT_SCLK = HardwarePins::TFT_SCLK;
constexpr int PIN_TFT_MOSI = HardwarePins::TFT_MOSI;
constexpr int PIN_TFT_CS = HardwarePins::TFT_CS;
constexpr int PIN_TFT_DC = HardwarePins::TFT_DC;
constexpr int PIN_TFT_RST = HardwarePins::TFT_RST;
constexpr int PIN_TFT_BACKLIGHT = HardwarePins::TFT_BACKLIGHT;
// ADC1 is required while Wi-Fi is active. GPIO1 can be used for the battery
// divider only when neither volume nor brightness uses the potentiometer.
constexpr int PIN_BATTERY_ADC = HardwarePins::SHARED_ADC;
constexpr bool BATTERY_MONITOR_ENABLED = false;
constexpr int PIN_BRIGHTNESS_POT = HardwarePins::SHARED_ADC;
constexpr uint8_t MIN_BRIGHTNESS = 5;

// Keep the original three page bits stable for saved settings and clients.
constexpr uint8_t PAGE_HOME = 0x01;
constexpr uint8_t PAGE_FORECAST = 0x02;
constexpr uint8_t PAGE_MEMOS = 0x04;
constexpr uint8_t PAGE_EXPRESSION = 0x08;
constexpr uint8_t PAGE_PHOTO = 0x10;
constexpr uint8_t PAGE_ALL = 0x1F;
constexpr uint8_t DEFAULT_PAGES = 0x0F;

enum class Scene : uint8_t {
  Booting,
  Connecting,
  Idle,
  Listening,
  Thinking,
  Speaking,
  Alarm,
  Error,
  Updating,
};

bool begin();
bool ready();
Panel currentPanel();
uint8_t currentRotation();
uint16_t width();
uint16_t height();
uint16_t contentTop();
bool saveRotationPreference(uint8_t rotation);
const char *panelId(Panel panel);
const char *panelLabel(Panel panel);
bool panelFromId(const String &id, Panel &panel);
bool savePanelPreference(Panel panel);
// Brightness applies immediately; only explicit saves write NVS. The 7-pin
// ST7789 dims RGB pixels because its backlight cannot be controlled separately.
bool saveBrightnessPreference(uint8_t percent, bool potentiometer);
uint8_t brightnessSetting();
uint8_t effectiveBrightness();
bool potentiometerBrightnessEnabled();
bool hardwareBacklightControl();
bool saveVoiceLayoutPreferences(bool buttonFullscreen, bool wakeFullscreen);
bool voiceFullscreen(bool wakeTriggered);
bool portraitBodyEnabled();
bool savePortraitBodyPreference(bool enabled);
void setVoiceSource(bool wakeTriggered);
uint64_t expressionMask();
bool saveExpressionMask(uint64_t mask);
bool previewExpression(uint8_t id);  // Eight seconds, never saves or interrupts a task.
void cancelExpressionPreview();
// Remains visible through startup/retry status changes, until the AP closes.
void setProvisioning(bool active, const String &ssid = "", const String &password = "",
                     const String &url = "", bool phoneConnected = false);
void show(Scene scene, const String &title, const String &body = "");
void setConversation(const String &question, const String &answer);
void clearConversation();
void setDialogueExpression(uint8_t id);
void setFocusStatus(bool active, const String &status);
void setWeather(const String &city, float temperatureC, const String &condition, bool valid);
void setForecast(const String dayLabels[3], const String conditions[3],
                 const int highC[3], const int lowC[3], size_t count);
void setWeatherStatus(const String &city, const String &status);
void setAlarmStatus(const String &status);
void setMemoStatus(const String &notes, const String &todos, size_t noteCount,
                   size_t todoCount, size_t pendingTodoCount);
void setPageMask(uint8_t pageMask);
void setPhotoMode(bool enabled);
void setNetwork(bool connected, int rssi, const String &ip = "");
void setSuspended(bool suspended);
float batteryVoltage();
int batteryPercent();

}  // namespace RobotDisplay
