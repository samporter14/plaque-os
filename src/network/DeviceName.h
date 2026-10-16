#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// What the reader calls itself on a network: the hotspot it opens ("Plaque-A1B2") and the name it asks routers for and
// answers to on .local ("plaque-a1b2"). The four digits are the last two bytes of the station MAC, the same last four
// digits Settings > About shows, so two readers in one room are told apart. A reader whose MAC could not be read (all
// zeros) is just "Plaque" / "plaque". Covered by test/device_name; the device side is in DeviceNameDevice.cpp.
namespace devicename {

constexpr size_t SSID_MAX_CHARS = 32;   // 802.11
constexpr size_t LABEL_MAX_CHARS = 63;  // one DNS label

// "Plaque-A1B2". Uppercase hex, as About shows the MAC.
std::string hotspotSsid(const uint8_t mac[6]);
// "plaque-a1b2": lowercase letters, digits and hyphens, which is all a DNS label (and a DHCP host name) may hold.
std::string hostname(const uint8_t mac[6]);

// The device side: the same two names for this reader, from its station MAC, read once.
const std::string& stationHotspotSsid();
const std::string& stationHostname();
// Gives the station its name for the next join. Call it before WiFi.mode(WIFI_STA) and again before WiFi.begin(): the
// framework only copies the name into the STA interface as the radio turns on, so it is also set on the interface
// itself when that already exists.
void applyStationHostname();

}  // namespace devicename
