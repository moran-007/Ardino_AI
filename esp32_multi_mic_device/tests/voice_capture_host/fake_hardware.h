#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>

namespace FakeHardware {
inline std::map<std::string, std::vector<uint8_t>> files;
inline std::function<int16_t(size_t)> sampleAt;
inline size_t inputSamples = 0;
inline size_t readCalls = 0;
inline size_t writtenFrames = 0;
inline size_t failReadAt = std::numeric_limits<size_t>::max();
inline size_t failFileWriteAfter = std::numeric_limits<size_t>::max();
inline uint32_t fileWriteDelayMs = 0;
inline size_t firstFileWriteInput = std::numeric_limits<size_t>::max();
inline size_t fileWriteCalls = 0;
inline bool failCaptureAllocation = false;
inline unsigned captureAllocationCaps = 0;
inline size_t abortAfterSamples = std::numeric_limits<size_t>::max();
inline size_t releaseAfterSamples = std::numeric_limits<size_t>::max();
inline bool failOpen = false;
inline bool failI2sBegin = false;
inline bool failI2sWrite = false;
inline size_t maxWriteBytes = SIZE_MAX;
inline bool abortNow = false;
inline bool talkDown = true;
inline bool mutexTaken = false;
inline size_t serviceCalls = 0;
inline bool failNs=false,failVad=false;
inline int liveNs=0,liveVad=0,vadMode=-1,nsDivisor=1;
inline size_t nsCalls=0,vadCalls=0;
inline std::vector<bool> vadFrames;
inline size_t maxReadFrames=std::numeric_limits<size_t>::max();
inline std::function<int16_t(size_t)> rightSampleAt, thirdSampleAt;
inline size_t thirdInputSamples = 0, thirdReadCalls = 0;
inline size_t thirdFailReadAt = std::numeric_limits<size_t>::max();
inline bool failThirdBegin = false;
inline int malformedPort = -1;
inline bool portActive[2] = {};
inline uint32_t portRate[2] = {};
inline size_t beginCalls[2] = {}, endCalls[2] = {};
inline int portPins[2][4] = {};
}
