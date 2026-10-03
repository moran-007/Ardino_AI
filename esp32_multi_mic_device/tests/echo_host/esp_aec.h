#pragma once
#include <cstdint>
#include <cstdlib>
#include <cassert>
enum { AEC_MODE_SR_LOW_COST=0, AEC_MODE_SR_HIGH_PERF=1, AEC_NLP_LEVEL_NORMAL=0 };
struct aec_config_t { int mic_num,ref_num,out_num,filter_length,sample_rate; uint32_t caps; int mode,nlp_level; };
struct aec_handle_t { int value; };
inline bool failAec = false;
inline int fakeChunk = 512, destroyedAec = 0, processedAec = 0;
inline aec_config_t lastConfig;
inline aec_handle_t *aec_create_from_config(aec_config_t *cfg) {
  lastConfig = *cfg; return failAec ? nullptr : new aec_handle_t{};
}
inline int aec_get_chunksize(aec_handle_t *) { return fakeChunk; }
inline void aec_destroy(aec_handle_t *h) { ++destroyedAec; delete h; }
inline void aec_process(aec_handle_t *, int16_t *in, int16_t *ref, int16_t *out) {
  assert(!(reinterpret_cast<uintptr_t>(in) % 16));
  assert(!(reinterpret_cast<uintptr_t>(ref) % 16));
  assert(!(reinterpret_cast<uintptr_t>(out) % 16));
  // Fake DSP checks signal routing only, not the vendor's acoustic algorithm.
  for (int i=0;i<512;++i) out[i]=in[i]-ref[i]/4;
  ++processedAec;
}
