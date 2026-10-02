#pragma once
#include "Arduino.h"
struct esp_partition_t {};
inline bool officialPartitionPresent = false;
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 2, ESP_OK = 0;
inline const esp_partition_t *esp_partition_find_first(int, int, const char *) {
  static esp_partition_t partition;
  return officialPartitionPresent ? &partition : nullptr;
}
inline int esp_partition_read(const esp_partition_t *, size_t, void *out, size_t count) {
  const uint32_t models = 13;memcpy(out, &models, count);return ESP_OK;
}
