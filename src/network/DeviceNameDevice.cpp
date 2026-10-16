#include <Logging.h>
#include <WiFi.h>
#include <esp_mac.h>

#include "DeviceName.h"

namespace devicename {
namespace {
// The station MAC does not change while the reader runs, so each name is made once. The one About shows is the
// station MAC too.
void readMac(uint8_t mac[6]) {
  const esp_err_t result = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (result != ESP_OK) {
    LOG_ERR("NAME", "Failed to read station MAC for the network name (err=%d)", static_cast<int>(result));
    for (int i = 0; i < 6; i++) mac[i] = 0;
  }
}
}  // namespace

const std::string& stationHotspotSsid() {
  static const std::string name = [] {
    uint8_t mac[6] = {};
    readMac(mac);
    return hotspotSsid(mac);
  }();
  return name;
}

const std::string& stationHostname() {
  static const std::string name = [] {
    uint8_t mac[6] = {};
    readMac(mac);
    return hostname(mac);
  }();
  return name;
}

void applyStationHostname() {
  const char* name = stationHostname().c_str();
  WiFi.setHostname(name);
  WiFi.STA.setHostname(name);  // false while the STA interface does not exist; the line above covers that case
}

}  // namespace devicename
