#pragma once
// The part of the ESP-IDF Wi-Fi driver API the join code calls, over the fake radio (FakeRadio.h). The enumerations
// are the installed IDF 5.5.5's (esp_wifi_types_generic.h), in its order.
#include <esp_err.h>

#include <cstdint>

typedef enum {
  WIFI_AUTH_OPEN = 0,
  WIFI_AUTH_WEP,
  WIFI_AUTH_WPA_PSK,
  WIFI_AUTH_WPA2_PSK,
  WIFI_AUTH_WPA_WPA2_PSK,
  WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA2_ENTERPRISE = WIFI_AUTH_ENTERPRISE,
  WIFI_AUTH_WPA3_PSK,
  WIFI_AUTH_WPA2_WPA3_PSK,
  WIFI_AUTH_WAPI_PSK,
  WIFI_AUTH_OWE,
  WIFI_AUTH_WPA3_ENT_192,
  WIFI_AUTH_WPA3_EXT_PSK,
  WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE,
  WIFI_AUTH_DPP,
  WIFI_AUTH_WPA3_ENTERPRISE,
  WIFI_AUTH_WPA2_WPA3_ENTERPRISE,
  WIFI_AUTH_WPA_ENTERPRISE,
  WIFI_AUTH_MAX
} wifi_auth_mode_t;

typedef enum { WIFI_FAST_SCAN = 0, WIFI_ALL_CHANNEL_SCAN } wifi_scan_method_t;
typedef enum { WIFI_CONNECT_AP_BY_SIGNAL = 0, WIFI_CONNECT_AP_BY_SECURITY } wifi_sort_method_t;
typedef enum { WIFI_IF_STA = 0, WIFI_IF_AP } wifi_interface_t;
typedef enum { WIFI_MODE_NULL = 0, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA } wifi_mode_t;
constexpr wifi_mode_t WIFI_OFF = WIFI_MODE_NULL;
constexpr wifi_mode_t WIFI_STA = WIFI_MODE_STA;

// wifi_err_reason_t, the values the code and the tests name.
constexpr int WIFI_REASON_ASSOC_LEAVE = 8;
constexpr int WIFI_REASON_NO_AP_FOUND = 201;
constexpr int WIFI_REASON_AUTH_FAIL = 202;
constexpr int WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD = 211;

typedef struct {
  int8_t rssi;
  wifi_auth_mode_t authmode;
  uint8_t rssi_5g_adjustment;
} wifi_scan_threshold_t;

typedef struct {
  uint8_t ssid[32];
  uint8_t password[64];
  wifi_scan_method_t scan_method;
  bool bssid_set;
  uint8_t bssid[6];
  uint8_t channel;
  wifi_sort_method_t sort_method;
  wifi_scan_threshold_t threshold;
} wifi_sta_config_t;

typedef union {
  wifi_sta_config_t sta;
} wifi_config_t;

typedef struct {
  uint8_t bssid[6];
  uint8_t ssid[33];
  uint8_t primary;
  int8_t rssi;
  wifi_auth_mode_t authmode;
} wifi_ap_record_t;

typedef enum { WIFI_PS_NONE, WIFI_PS_MIN_MODEM, WIFI_PS_MAX_MODEM } wifi_ps_type_t;
esp_err_t esp_wifi_get_ps(wifi_ps_type_t* type);
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type);

esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t* conf);
esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* conf);
esp_err_t esp_wifi_connect();
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap);
esp_err_t esp_wifi_sta_enterprise_enable();
esp_err_t esp_wifi_sta_enterprise_disable();
