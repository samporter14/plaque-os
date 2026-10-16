#pragma once
typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_SW, ESP_RST_DEEPSLEEP } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason();
