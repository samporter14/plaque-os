#pragma once
#include <esp_err.h>
enum wifi_ps_type_t { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM };
esp_err_t esp_wifi_get_ps(wifi_ps_type_t* type);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);
