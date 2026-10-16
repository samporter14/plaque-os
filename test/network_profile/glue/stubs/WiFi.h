#pragma once
// The Arduino-ESP32 Wi-Fi station API as the glue code uses it, over the fake radio. Behaviour follows the installed
// core (framework-arduinoespressif32 STA.cpp): begin(ssid) sets no auth threshold, begin(ssid, pass) sets the minimum
// security (WPA2-PSK unless changed), and only begin(..., tryConnect = true) connects.
#include <esp_err.h>
#include <esp_wifi.h>

#include <cstdint>
#include <string>

typedef enum {
  WL_NO_SHIELD = 255,
  WL_STOPPED = 254,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
} wl_status_t;

constexpr int16_t WIFI_SCAN_RUNNING = -1;
constexpr int16_t WIFI_SCAN_FAILED = -2;

class IPAddress {
  uint8_t octets[4] = {0, 0, 0, 0};

 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : octets{a, b, c, d} {}
  uint8_t operator[](int index) const { return octets[index]; }
  operator uint32_t() const {
    return static_cast<uint32_t>(octets[0]) | (static_cast<uint32_t>(octets[1]) << 8) |
           (static_cast<uint32_t>(octets[2]) << 16) | (static_cast<uint32_t>(octets[3]) << 24);
  }
};

typedef enum {
  ARDUINO_EVENT_WIFI_STA_CONNECTED = 0,
  ARDUINO_EVENT_WIFI_STA_DISCONNECTED,
  ARDUINO_EVENT_MAX
} arduino_event_id_t;

typedef struct {
  uint8_t channel;
  wifi_auth_mode_t authmode;
} wifi_event_sta_connected_t;
typedef struct {
  uint8_t reason;
} wifi_event_sta_disconnected_t;

typedef union {
  wifi_event_sta_connected_t wifi_sta_connected;
  wifi_event_sta_disconnected_t wifi_sta_disconnected;
} arduino_event_info_t;

typedef struct {
  arduino_event_id_t event_id;
  arduino_event_info_t event_info;
} arduino_event_t;

typedef void (*NetworkEventSysCb)(arduino_event_t* event);
typedef size_t network_event_handle_t;

class WiFiClass {
 public:
  void persistent(bool enable);
  bool mode(wifi_mode_t mode);
  wifi_mode_t getMode();
  bool disconnect(bool wifioff = false, bool eraseap = false);
  wl_status_t begin(const char* ssid, const char* passphrase = nullptr, int32_t channel = 0,
                    const uint8_t* bssid = nullptr, bool connect = true);
  wl_status_t status();
  void setScanMethod(wifi_scan_method_t method);
  void setSortMethod(wifi_sort_method_t method);
  void setMinSecurity(wifi_auth_mode_t minSecurity);
  network_event_handle_t onEvent(NetworkEventSysCb callback, arduino_event_id_t event = ARDUINO_EVENT_MAX);
  bool removeEvent(network_event_handle_t handle);
  int16_t scanNetworks(bool async = false);
  int16_t scanComplete();
  void scanDelete();
  struct SsidText {
    std::string text;
    const char* c_str() const { return text.c_str(); }
    operator std::string() const { return text; }
  };
  SsidText SSID(uint8_t index);
  int32_t RSSI(uint8_t index);
  int8_t RSSI();
  uint8_t channel();
  IPAddress localIP();
  IPAddress gatewayIP();
  IPAddress dnsIP(uint8_t index = 0);
};

extern WiFiClass WiFi;
