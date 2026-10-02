#pragma once

#include <Adafruit_GFX.h>
#include "robot_display.h"
#include "robot_expression_catalog.h"

namespace RobotAvatar {
// All positions are relative to this reserved rectangle; no framebuffer or
// image assets are allocated. The same expressions work at both native sizes.
constexpr uint8_t EXPRESSION_COUNT = RobotExpressions::COUNT;
void draw(Adafruit_GFX &canvas, RobotDisplay::Scene scene, bool fullBody,
          int x, int y, int width, int height, uint32_t now, uint8_t expression = 0);
}  // namespace RobotAvatar
