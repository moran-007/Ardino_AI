#pragma once
struct srmodel_list_t { int num; char **model_name; };
inline srmodel_list_t *esp_srmodel_init(const char *) {
  static char model[] = "wn9_nihaoxiaozhi_tts";
  static char *names[] = {model};
  static srmodel_list_t models = {1, names};return &models;
}
