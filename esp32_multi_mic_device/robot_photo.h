#pragma once

#include <Arduino.h>
#include <FS.h>
#include <WebServer.h>

namespace RobotPhoto {
constexpr uint8_t MAX_PHOTOS = 8;
struct CommandResult {
  bool handled = false;
  bool changed = false;
  bool showPhoto = false;
  String message;
};
// Browser-prepared RGB565 data is streamed to storage, then copied a row at a
// time into the display's existing framebuffer. Both panel sizes and portrait /
// landscape orientations are supported. Center-crop fills the whole screen
// with a uniform scale. No JPEG decoder or additional framebuffer is kept.
bool begin(fs::FS &storage, uint16_t viewportWidth, uint16_t viewportHeight);
void registerRoutes(WebServer &server, bool (*canModify)(), void (*onSelection)());
bool available();
uint32_t revision();
bool select(uint8_t id);
bool step(int direction);
CommandResult handleNaturalLanguage(const String &input);
bool copyTo(uint16_t *pixels, uint16_t canvasWidth, uint16_t canvasHeight);
}  // namespace RobotPhoto
