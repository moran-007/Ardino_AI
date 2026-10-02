#pragma once

#include <Arduino.h>
#include <stddef.h>

// Central hardware pin map. Keep all signal assignments here so the display,
// controls and audio modules cannot silently drift out of sync.
namespace HardwarePins {

#if CONFIG_IDF_TARGET_ESP32S3
// ST7735S: use only the header row next to the display.
constexpr int TFT_SCLK = 4;
constexpr int TFT_MOSI = 5;
constexpr int TFT_RST = 6;
constexpr int TFT_DC = 7;
constexpr int TFT_CS = 15;
constexpr int TFT_BACKLIGHT = 16;

// INMP441 and MAX98357A: use the opposite header row. BCLK and WS are shared.
constexpr int I2S_BCLK = 42;
constexpr int I2S_WS = 41;
constexpr int MIC_DATA = 21;
// MIC1 = left and MIC2 = right on MIC_DATA. MIC3 has its own I2S RX bus.
constexpr int MIC3_BCLK = 18;
constexpr int MIC3_WS = 38;
constexpr int MIC3_DATA = 11;
constexpr int AMP_DATA = 40;
constexpr int AMP_SD = 39;

// External active-low buttons. GPIO0 remains dedicated to BOOT/recovery.
constexpr int TALK_BUTTON = 2;
constexpr int ABORT_BUTTON = 47;
constexpr int BOOT_BUTTON = 0;

// GPIO1 is an optional shared analog input: battery monitoring, volume knob,
// or brightness knob. These three uses are mutually exclusive.
constexpr int SHARED_ADC = 1;

// Four SG90 signal outputs; GPIO8..10 remain available for STEP/DIR/nSLEEP.
constexpr int SERVO_1 = 12;
constexpr int SERVO_2 = 13;
constexpr int SERVO_3 = 14;
constexpr int SERVO_4 = 17;
#else
// Legacy ESP32 fallback; this project is built and tested for ESP32-S3.
constexpr int TFT_SCLK = 12;
constexpr int TFT_MOSI = 11;
constexpr int TFT_CS = 10;
constexpr int TFT_DC = 9;
constexpr int TFT_RST = 8;
constexpr int TFT_BACKLIGHT = -1;
constexpr int I2S_BCLK = 26;
constexpr int I2S_WS = 25;
constexpr int MIC_DATA = 33;
constexpr int AMP_DATA = 22;
constexpr int AMP_SD = 27;
constexpr int TALK_BUTTON = 2;
constexpr int ABORT_BUTTON = 16;
constexpr int BOOT_BUTTON = 0;
constexpr int SHARED_ADC = 34;
constexpr int SERVO_1 = -1;
constexpr int SERVO_2 = -1;
constexpr int SERVO_3 = -1;
constexpr int SERVO_4 = -1;
#endif

constexpr int ACTIVE_SIGNAL_PINS[] = {
    TFT_SCLK, TFT_MOSI, TFT_CS, TFT_DC, TFT_RST, TFT_BACKLIGHT,
    I2S_BCLK, I2S_WS, MIC_DATA, AMP_DATA, AMP_SD,
    TALK_BUTTON, ABORT_BUTTON,
#if CONFIG_IDF_TARGET_ESP32S3
    SERVO_1, SERVO_2, SERVO_3, SERVO_4,
    MIC3_BCLK, MIC3_WS, MIC3_DATA,
#endif
};

template <size_t N>
constexpr bool pinsAreUnique(const int (&pins)[N]) {
  for (size_t i = 0; i < N; ++i) {
    for (size_t j = i + 1; j < N; ++j) {
      if (pins[i] == pins[j]) return false;
    }
  }
  return true;
}

#if CONFIG_IDF_TARGET_ESP32S3
constexpr bool isReservedEsp32S3Pin(int pin) {
  // GPIO0/3/45/46 are strapping pins; 19/20 are native USB; 33..37 are used
  // by OPI memory; 43/44 are UART0; GPIO48 is reserved for the onboard RGB.
  return pin < 0 || pin == 0 || pin == 3 || pin == 19 || pin == 20 ||
         (pin >= 33 && pin <= 37) || pin == 43 || pin == 44 ||
         pin == 45 || pin == 46 || pin == 48;
}

template <size_t N>
constexpr bool pinsAvoidReservedEsp32S3Pins(const int (&pins)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (isReservedEsp32S3Pin(pins[i])) return false;
  }
  return true;
}
#endif

static_assert(pinsAreUnique(ACTIVE_SIGNAL_PINS),
              "Hardware GPIO conflict: active signals must use unique pins");
#if CONFIG_IDF_TARGET_ESP32S3
static_assert(pinsAvoidReservedEsp32S3Pins(ACTIVE_SIGNAL_PINS),
              "Unsafe ESP32-S3 GPIO selected: check OPI PSRAM/USB/BOOT/UART/RGB reservations");
#endif

}  // namespace HardwarePins
