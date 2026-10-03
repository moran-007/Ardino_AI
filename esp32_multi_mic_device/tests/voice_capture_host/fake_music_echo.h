#pragma once
#include "music_echo_capture.h"
#include <functional>
namespace FakeEcho {
inline bool enabled=false, running=false, failBegin=false, failRead=false;
inline size_t nativeFrames=0, processCalls=0;
inline std::function<MusicEcho::NativeFrame(size_t)> source;
}
