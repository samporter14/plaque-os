#include <Arduino.h>
#include <WiFi.h>

#include <atomic>
#include <cstdio>

#include "ConnectionCheckDevice.h"
#include "WifiCredentialStore.h"
#include "network/NetworkProfileDevice.h"

// DeviceNetwork, the connection check's network port (see ConnectionCheckDevice.h). Apart from the rest of the check's
// device adapters so that the host tests can build it against a fake Wi-Fi layer (test/network_glue).
namespace conncheck {
namespace {
// What the driver said during the current join. The Arduino event task writes it; the main loop reads it.
std::atomic<bool> eventsWanted{false};
std::atomic<bool> associated{false};
std::atomic<int> lastReason{0};
std::atomic<int> lastChannel{0};

void onWifiEvent(arduino_event_t* event) {
  if (!event || !eventsWanted.load()) return;
  switch (event->event_id) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      // Associated, and for a protected network past the 4-way handshake (or EAP): the join is done.
      lastChannel.store(event->event_info.wifi_sta_connected.channel);
      associated.store(true);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      const int reason = event->event_info.wifi_sta_disconnected.reason;
      // The station's own leave (the disconnect before a join, or the one that ends it) says nothing about the network.
      if (reason == WIFI_REASON_ASSOC_LEAVE) break;
      lastReason.store(reason);
      associated.store(false);
      break;
    }
    default:
      break;
  }
}

void ipText(const IPAddress& address, char* out, const size_t size) {
  std::snprintf(out, size, "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
}

bool unset(const IPAddress& address) { return static_cast<uint32_t>(address) == 0; }
}  // namespace

// ---- network --------------------------------------------------------------------------------------------------------

DeviceNetwork::~DeviceNetwork() { off(); }

NetworkInfo DeviceNetwork::describe() {
  NetworkInfo info;
  // An installed profile is the configured network; DeviceWifi numbers it after the saved networks.
  const netprofile::Summary profile = netprofile::summary();
  if (profile.installed) {
    target = static_cast<int>(WIFI_STORE.getCredentialCount());
    info.configured = true;
    info.source = "profile";
    info.name = profile.name;
    info.ssid = profile.ssid;
    info.auth = netprofile::authName(profile.auth);
    return info;
  }
  target = wifi.lastUsed();
  if (target < 0 && wifi.savedCount() > 0) target = 0;
  if (target < 0) return info;
  // The password-free summary: whether a password exists is all the file says about it.
  const auto summaries = WIFI_STORE.getCredentialSummaries();
  if (static_cast<size_t>(target) >= summaries.size()) {
    target = -1;
    return info;
  }
  const auto& summary = summaries[static_cast<size_t>(target)];
  info.configured = true;
  info.source = "saved";
  info.name = summary.ssid;
  info.ssid = summary.ssid;
  info.auth = summary.hasPassword ? "psk" : "open";
  return info;
}

void DeviceNetwork::beginJoin() {
  associated.store(false);
  lastReason.store(0);
  lastChannel.store(0);
  eventsWanted.store(true);
  if (!listening) {
    eventHandle = WiFi.onEvent(static_cast<NetworkEventSysCb>(&onWifiEvent));
    listening = true;
  }
  if (target >= 0) wifi.beginConnect(static_cast<size_t>(target));
}

JoinState DeviceNetwork::joinState() {
  // Polled every pass: DeviceWifi starts the join itself once the radio has settled.
  const method::WifiStatus status = wifi.connectStatus();
  if (associated.load() || status == method::WifiStatus::Connected) return JoinState::Joined;
  if (status == method::WifiStatus::Failed) return JoinState::Failed;
  return JoinState::Pending;
}

int DeviceNetwork::disconnectReason() { return lastReason.load(); }

int DeviceNetwork::rssi() { return static_cast<int>(WiFi.RSSI()); }

int DeviceNetwork::channel() {
  const int seen = lastChannel.load();
  return seen > 0 ? seen : static_cast<int>(WiFi.channel());
}

bool DeviceNetwork::linkUp() { return associated.load() || WiFi.status() == WL_CONNECTED; }

AddressState DeviceNetwork::address(AddressInfo& out) {
  wifi.connectStatus();
  const IPAddress ip = WiFi.localIP();
  if (unset(ip)) return AddressState::None;
  ipText(ip, out.ip, sizeof(out.ip));
  const IPAddress gateway = WiFi.gatewayIP();
  if (!unset(gateway)) ipText(gateway, out.gateway, sizeof(out.gateway));
  out.dnsCount = 0;
  for (uint8_t i = 0; i < 2; ++i) {
    const IPAddress server = WiFi.dnsIP(i);
    if (!unset(server)) ipText(server, out.dns[out.dnsCount++], sizeof(out.dns[0]));
  }
  return !unset(gateway) && out.dnsCount > 0 ? AddressState::Complete : AddressState::Partial;
}

void DeviceNetwork::off() {
  eventsWanted.store(false);
  if (listening) {
    WiFi.removeEvent(static_cast<network_event_handle_t>(eventHandle));
    listening = false;
    eventHandle = 0;
  }
  wifi.abortConnect();
  wifi.off();
  associated.store(false);
}

}  // namespace conncheck
