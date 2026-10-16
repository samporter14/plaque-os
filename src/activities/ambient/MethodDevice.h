#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ambient/MethodPorts.h"
#include "network/HttpDownloader.h"
#include "network/NetworkJoin.h"

class GfxRenderer;
class HalFile;

// The reader's side of the Method ports (src/ambient/MethodPorts.h): Arduino Wi-Fi, the TLS clock,
// esp_http_client, the SD card through HalStorage, mbedtls and the battery gauge. Each adapter is a thin
// translation; the decisions live in the pure code that test/method_* covers.
namespace method {

class DeviceClock final : public ClockPort {
  SyncStatus outcome = SyncStatus::Pending;  // latched once a poll has seen the sync end

 public:
  uint32_t nowMs() override;
  int64_t unixNow() override;
  bool localTime(int64_t epoch, LocalTime& out) override;
  bool clockReady() override;
  void beginSync() override;
  SyncStatus syncStatus() override;
  void abortSync() override;
  bool useRetained() override;
};

// The saved networks keep their WifiCredentialStore indices; an installed network profile is the index one past them,
// and network/NetworkJoin decides the order (profile first, then the saved networks as always).
class DeviceWifi final : public WifiPort {
  enum class Step : uint8_t { None, Settling, Active };
  Step connectStep = Step::None;
  Step scanStep = Step::None;
  uint32_t settleUntil = 0;
  std::string ssid;
  std::string password;
  std::vector<size_t> visible;
  std::string pendingNote;  // the network joined, still to be remembered as the one to try first
  netjoin::Mode mode = netjoin::Mode::Manual;
  netjoin::Situation situation;
  std::string profileSsid;
  bool profileAttempt = false;  // the join in progress is the profile's
  bool scanSkipped = false;     // a wake does not scan: the order is known without one

  void startJoin();
  void readSituation();
  int savedLastUsed() const;

 public:
  // A timer wake tries the profile and the last-used network only, and never scans.
  void setMode(netjoin::Mode joinMode) { mode = joinMode; }
  bool connected() override;
  size_t savedCount() override;
  int lastUsed() override;
  void beginConnect(size_t saved) override;
  WifiStatus connectStatus() override;
  void abortConnect() override;
  void beginScan() override;
  ScanStatus scanStatus() override;
  std::vector<size_t> visibleSaved() override { return visible; }
  void noteJoined(size_t saved) override;
  // Writes the remembered network once the render lock can be had without waiting. The shelf calls it every loop
  // pass; it does nothing when there is nothing pending.
  void writePendingNote();
  void off() override;
};

class DeviceHttp final : public HttpPort {
  GfxRenderer& renderer;
  HttpDownloader::BoundedGet get;
  uint32_t lookupStartedAt = 0;
  bool lookupLogged = true;
  bool refused = false;  // the last open() was refused before a request was made

 public:
  explicit DeviceHttp(GfxRenderer& renderer);
  void beginResolve() override;
  ResolveStatus resolveStatus() override;
  void abortResolve() override;
  HttpReady ready() override;
  HttpOpen open(const char* path, size_t maxBytes) override;
  int64_t contentLength() override { return get.contentLength(); }
  HttpRead read(uint8_t* buffer, size_t capacity, size_t& got) override;
  void close() override { get.close(); }
  // What the last open() found, told apart (see HttpFailure.h). The shelf's results do not use it; the connection
  // check reports it.
  httpfail::Outcome outcome() const;
};

class DeviceFiles final : public FilePort {
  std::unique_ptr<HalFile> sink;

 public:
  DeviceFiles();
  ~DeviceFiles() override;
  bool exists(const char* path) override;
  bool readAll(const char* path, size_t maxBytes, std::string& out) override;
  int64_t fileSize(const char* path) override;
  bool writeFile(const char* path, std::string_view data) override;
  bool replaceFile(const char* tmpPath, const char* destPath) override;
  bool remove(const char* path) override;
  bool ensureDirectory(const char* path) override;
  std::vector<std::string> listBooks(const char* directory) override;
  bool inspectBook(const char* path, BookInfo& info) override;
  int64_t modifiedAt(const char* path) override;
  int64_t savedPage(const char* bookPath) override;
  void removeBook(const char* bookPath) override;
  void removeBookCache(const char* bookPath) override;
  uint64_t freeBytes() override;
  bool openForWrite(const char* path) override;
  bool write(const uint8_t* data, size_t size) override;
  bool closeWrite() override;
};

class DeviceHash final : public HashPort {
  struct Context;
  std::unique_ptr<Context> context;

 public:
  DeviceHash();
  ~DeviceHash() override;
  bool begin() override;
  bool update(const uint8_t* data, size_t size) override;
  bool finish(char out[65]) override;
  void abandon() override;
};

class DeviceBattery final : public BatteryPort {
 public:
  int percent() override;
};

// Serial log: each phase with its duration, free heap and the largest free block.
class DeviceLog final : public Observer {
 public:
  void phaseEntered(const char* name) override;
  void phaseLeft(const char* name, uint32_t elapsedMs, const char* outcome, uint32_t slowestPassMs) override;
};

// Everything the shelf and its check use, created together.
struct DevicePorts {
  DeviceClock clock;
  DeviceWifi wifi;
  DeviceHttp http;
  DeviceFiles files;
  DeviceHash hash;
  DeviceBattery battery;
  DeviceLog log;

  explicit DevicePorts(GfxRenderer& renderer) : http(renderer) {}
};

}  // namespace method
