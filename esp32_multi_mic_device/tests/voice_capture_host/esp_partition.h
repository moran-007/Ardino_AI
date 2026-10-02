#pragma once
constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 2;
constexpr int ESP_PARTITION_SUBTYPE_DATA_FAT = 3;
struct esp_partition_t {};
inline const esp_partition_t *esp_partition_find_first(int, int subtype, const char *) {
  static esp_partition_t partition;
  return subtype == ESP_PARTITION_SUBTYPE_DATA_SPIFFS ? &partition : nullptr;
}
