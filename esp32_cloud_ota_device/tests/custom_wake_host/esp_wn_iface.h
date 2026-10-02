#pragma once
#include "Arduino.h"
struct model_iface_data_t {};
enum wakenet_state_t { WAKENET_CHANNEL_VERIFIED = -1, WAKENET_NO_DETECT = 0, WAKENET_DETECTED = 1 };
constexpr int DET_MODE_90 = 90;
struct esp_wn_iface_t {
  model_iface_data_t *(*create)(const char *, int);
  void (*destroy)(model_iface_data_t *);
  int (*get_samp_rate)(model_iface_data_t *);
  int (*get_channel_num)(model_iface_data_t *);
  int (*get_samp_chunksize)(model_iface_data_t *);
  wakenet_state_t (*detect)(model_iface_data_t *, const int16_t *);
};
