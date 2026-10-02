#pragma once
#include <cstdint>
#include "qr_fixtures.h"
using esp_qrcode_handle_t=const QrFixture*;
struct esp_qrcode_config_t {void(*display_func)(esp_qrcode_handle_t);int max_qrcode_version;int qrcode_ecc_level;};
#define ESP_OK 0
#define ESP_QRCODE_ECC_MED 1
inline int qrEncodeCalls=0;
inline bool failQr=false;
inline int esp_qrcode_get_size(esp_qrcode_handle_t q){return q->size;}
inline bool esp_qrcode_get_module(esp_qrcode_handle_t q,int x,int y){return q->modules[y*q->size+x]=='1';}
inline int esp_qrcode_generate(esp_qrcode_config_t *config,const char *payload){
  ++qrEncodeCalls;
  if(failQr)return -1;
  for(const auto &qr:qrFixtures)if(std::strcmp(qr.payload,payload)==0){config->display_func(&qr);return ESP_OK;}
  return -1;
}
