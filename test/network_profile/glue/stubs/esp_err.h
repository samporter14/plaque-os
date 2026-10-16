#pragma once
// The ESP-IDF error type, as far as the glue code uses it.
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_ARG = 0x102;
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 0x1102;
constexpr esp_err_t ESP_ERR_NVS_NOT_ENOUGH_SPACE = 0x110d;
constexpr esp_err_t ESP_ERR_NVS_INVALID_LENGTH = 0x110c;
