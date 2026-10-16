#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "activities/ambient/MethodDevice.h"
#include "network/ConnectionCheck.h"

class GfxRenderer;

// The reader's side of the connection check's ports (src/network/ConnectionCheck.h). The join goes through the Method
// shelf's method::DeviceWifi, by its public interface only; the clock through method::DeviceClock and
// message_tls; the name lookup and the request through method::DeviceHttp. What DeviceWifi does not tell (associated
// as opposed to addressed, the driver's disconnect reason, the channel) comes from an Arduino Wi-Fi event observer
// registered here for the length of a join.
namespace conncheck {

// The network the check joins. For now: the saved network joined last, else the first one saved, described from the
// saved-network store ("saved"; "psk" when it has a password, else "open"). The installed network profile replaces
// this when it exists.
class DeviceNetwork final : public NetworkPort {
  method::DeviceWifi wifi;
  int target = -1;  // index in the saved-network store
  bool listening = false;
  size_t eventHandle = 0;

 public:
  DeviceNetwork() = default;
  ~DeviceNetwork() override;
  DeviceNetwork(const DeviceNetwork&) = delete;
  DeviceNetwork& operator=(const DeviceNetwork&) = delete;

  NetworkInfo describe() override;
  void beginJoin() override;
  JoinState joinState() override;
  int disconnectReason() override;
  int rssi() override;
  int channel() override;
  int eapTimeChecked() override { return -1; }
  bool linkUp() override;
  AddressState address(AddressInfo& out) override;
  void off() override;
};

class DeviceTime final : public TimePort {
  method::DeviceClock clock;

 public:
  uint32_t nowMs() override { return clock.nowMs(); }
  void beginSync() override { clock.beginSync(); }
  method::SyncStatus syncStatus() override { return clock.syncStatus(); }
  void abortSync() override { clock.abortSync(); }
  bool useRetained() override;
};

class DeviceMailbox final : public HttpPort {
  method::DeviceHttp http;

 public:
  explicit DeviceMailbox(GfxRenderer& renderer) : http(renderer) {}
  void beginResolve() override { http.beginResolve(); }
  method::ResolveStatus resolveStatus() override { return http.resolveStatus(); }
  void abortResolve() override { http.abortResolve(); }
  method::HttpReady ready() override { return http.ready(); }
  method::HttpOpen open(const char* path, size_t maxBytes) override { return http.open(path, maxBytes); }
  int64_t contentLength() override { return http.contentLength(); }
  method::HttpRead read(uint8_t* buffer, size_t capacity, size_t& got) override {
    return http.read(buffer, capacity, got);
  }
  void close() override { http.close(); }
  httpfail::Outcome outcome() override { return http.outcome(); }
};

// Serial log: each stage with its outcome, duration, free heap and largest free block.
class DeviceLog final : public Observer {
 public:
  void stageEnded(StageId stage, Status status, Detail detail, uint32_t ms) override;
};

struct DevicePorts {
  DeviceNetwork network;
  DeviceTime time;
  DeviceMailbox http;
  DeviceLog log;

  explicit DevicePorts(GfxRenderer& renderer) : http(renderer) {}
};

// The mailbox as the check sees it, from the Messages settings.
Config mailboxConfig();

// Writes /Lamina/connection-check.json through HalStorage (a temporary file, then a replace). Not while USB Drive owns
// the card, or with no card: then false, and nothing is written.
bool writeResult(const Report& report, const Config& config);

}  // namespace conncheck
