#pragma once
#include <Arduino.h>
#include <driver/i2s_std.h>
#include "multi_mic_selector.h"

// Captures the actual DMA output, rather than audio still queued for playback.
// The music-only RX path pairs that reference with the microphone DMA frame.
namespace MusicEcho {
struct NativeFrame { int16_t mic[3]; int16_t reference; };
struct Diagnostics {
  bool active = false;
  uint32_t frames = 0, droppedFrames = 0, missingReferenceFrames = 0;
  uint32_t processMaxUs = 0;
  uint16_t inputRms = 0, referenceRms = 0, outputRms = 0;
};
bool begin(i2s_chan_handle_t tx, i2s_chan_handle_t rx, i2s_chan_handle_t third, uint8_t installedMask = 1);
// Called only after all I2S channels have been deleted and the wake task paused.
void end();
// Wake resumption first drains old hardware RX; then discard this paired queue
// and reset diagnostics before measuring the new listening interval.
void discardPending();
void setMuted(bool muted);
bool active();
bool readNative(NativeFrame *frames, size_t count, uint32_t timeoutMs);
size_t frameSize();
// Buffers must be 16-byte aligned; cancellation precedes microphone gain.
void process(int16_t *near, int16_t *reference, int16_t *out);
Diagnostics diagnostics();
const char *error();
}
