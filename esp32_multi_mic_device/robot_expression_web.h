#pragma once
#include <WebServer.h>

namespace ExpressionWeb {
// Thumbnail uses the same production renderer as the physical display.
void sendThumbnail(WebServer &server, uint8_t id, bool body);
void sendCatalog(WebServer &server, uint64_t enabled);
}
