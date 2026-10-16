#pragma once

// Fake ports and helpers shared by the Method host tests (method_library, method_check).
// No Arduino, no ESP-IDF: the same pure code that runs on the reader runs here.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "MethodCheck.h"
#include "MethodFormat.h"
#include "MethodLibrary.h"
#include "MethodPorts.h"
#include "MethodProtocol.h"

namespace fake {

// ---- test harness ---------------------------------------------------------------------
inline std::ostream& operator<<(std::ostream& out, const std::vector<std::string>& list) {
  out << '[';
  for (size_t i = 0; i < list.size(); ++i) out << (i ? ", " : "") << '"' << list[i] << '"';
  return out << ']';
}
inline std::ostream& operator<<(std::ostream& out, const std::vector<std::pair<uint32_t, uint32_t>>& list) {
  out << '[';
  for (size_t i = 0; i < list.size(); ++i) out << (i ? ", " : "") << list[i].first << '/' << list[i].second;
  return out << ']';
}
inline int& checkCount() {
  static int count = 0;
  return count;
}
inline void check(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checkCount();
}
template <typename A, typename B>
inline void checkEq(const A& actual, const B& expected, const char* name) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << name << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
    std::exit(1);
  }
  ++checkCount();
}

// ---- SHA-256 (the firmware uses mbedtls; the tests need a reference) -----------------
inline std::string sha256Hex(const std::string& data) {
  static const uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::string message = data;
  const uint64_t bitLength = static_cast<uint64_t>(data.size()) * 8;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) message.push_back('\0');
  for (int i = 7; i >= 0; --i) message.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xFF));
  const auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
  for (size_t offset = 0; offset < message.size(); offset += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4])) << 24) |
             (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4 + 1])) << 16) |
             (static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4 + 2])) << 8) |
             static_cast<uint32_t>(static_cast<uint8_t>(message[offset + i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
      const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const uint32_t ch = (e & f) ^ (~e & g);
      const uint32_t t1 = hh + s1 + ch + K[i] + w[i];
      const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const uint32_t t2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }
  char hex[65];
  for (int i = 0; i < 8; ++i) snprintf(hex + i * 8, 9, "%08x", h[i]);
  return hex;
}

// ---- calendar helper (tests only) -----------------------------------------------------
// Converts a unix time plus a fixed UTC offset to a LocalTime without any C library time zone.
inline method::LocalTime localFromEpoch(int64_t epoch, int offsetSeconds) {
  const int64_t local = epoch + offsetSeconds;
  int64_t days = local / 86400;
  int64_t secs = local % 86400;
  if (secs < 0) {
    secs += 86400;
    --days;
  }
  method::LocalTime out;
  out.hour = static_cast<int>(secs / 3600);
  out.minute = static_cast<int>((secs % 3600) / 60);
  out.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday (4)
  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  out.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  out.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  out.year = static_cast<int>(y + (out.month <= 2));
  out.valid = true;
  return out;
}

// ---- files ----------------------------------------------------------------------------
struct FakeFiles : method::FilePort {
  std::map<std::string, std::string> files;
  std::map<std::string, int64_t> modified;
  std::map<std::string, int64_t> progress;        // saved page per book path
  std::map<std::string, uint32_t> bookPages;      // parsed page count per file (default 8)
  std::map<std::string, std::string> bookTitles;  // parsed title per file (default: the file stem)
  std::set<std::string> unparsable;
  std::set<std::string> failWrite;        // writeFile(path) fails
  std::set<std::string> failReplaceDest;  // replaceFile(_, dest) fails before it changes anything
  std::set<std::string> failRenameDest;   // replaceFile(_, dest) removes dest, then the rename fails (tmp stays)
  std::set<std::string> failRead;         // readAll(path) fails although the file is there
  std::vector<std::string> log;
  uint64_t free = 0;
  bool failEnsure = false;
  bool failOpenSink = false;
  bool failSinkWrite = false;
  bool failCloseSink = false;
  std::string sinkPath;
  bool sinkOpen = false;

  bool has(const std::string& path) const { return files.count(path) != 0; }
  void put(const std::string& path, const std::string& bytes, int64_t mtime = 0) {
    files[path] = bytes;
    modified[path] = mtime;
  }
  int count(const std::string& entry) const { return static_cast<int>(std::count(log.begin(), log.end(), entry)); }
  size_t indexOfLog(const std::string& entry) const {
    const auto it = std::find(log.begin(), log.end(), entry);
    return it == log.end() ? SIZE_MAX : static_cast<size_t>(it - log.begin());
  }

  bool exists(const char* path) override { return files.count(path) != 0; }
  bool readAll(const char* path, size_t maxBytes, std::string& out) override {
    // As HalStorage::readFileToString does: a zero-byte file reads as a failure, like a missing or oversized one.
    const auto it = files.find(path);
    if (it == files.end() || it->second.empty() || it->second.size() > maxBytes || failRead.count(path)) return false;
    out = it->second;
    return true;
  }
  int64_t fileSize(const char* path) override {
    const auto it = files.find(path);
    return it == files.end() ? -1 : static_cast<int64_t>(it->second.size());
  }
  bool writeFile(const char* path, std::string_view data) override {
    log.push_back(std::string("write ") + path);
    if (failWrite.count(path)) return false;
    files[path] = std::string(data);
    return true;
  }
  bool replaceFile(const char* tmp, const char* dest) override {
    log.push_back(std::string("replace ") + tmp + " " + dest);
    if (failReplaceDest.count(dest) || !files.count(tmp)) return false;
    // As the card does it: FAT rename does not replace a file, so the old one is removed first.
    files.erase(dest);
    if (failRenameDest.count(dest)) return false;
    files[dest] = files[tmp];
    files.erase(tmp);
    return true;
  }
  bool remove(const char* path) override {
    log.push_back(std::string("remove ") + path);
    return files.erase(path) != 0;
  }
  bool ensureDirectory(const char* path) override {
    log.push_back(std::string("mkdir ") + path);
    return !failEnsure;
  }
  std::vector<std::string> listBooks(const char* directory) override {
    std::vector<std::string> names;
    const std::string prefix = std::string(directory) + "/";
    for (const auto& file : files) {
      if (file.first.compare(0, prefix.size(), prefix) != 0) continue;
      const std::string name = file.first.substr(prefix.size());
      if (name.find('/') == std::string::npos && name.size() > 4 && name.substr(name.size() - 4) == ".xtc")
        names.push_back(name);
    }
    return names;
  }
  bool inspectBook(const char* path, method::BookInfo& info) override {
    const auto it = files.find(path);
    if (it == files.end()) return false;
    info = {};
    info.bytes = it->second.size();
    info.magicOk = it->second.compare(0, 3, "XTC") == 0;
    info.parsed = info.magicOk && !unparsable.count(path);
    const auto pages = bookPages.find(path);
    info.pageCount = pages == bookPages.end() ? 8 : pages->second;
    const auto title = bookTitles.find(path);
    if (title != bookTitles.end()) {
      info.title = title->second;
    } else {
      std::string stem = path;
      stem = stem.substr(stem.find_last_of('/') + 1);
      info.title = stem.substr(0, stem.size() - 4);
    }
    return true;
  }
  int64_t modifiedAt(const char* path) override {
    const auto it = modified.find(path);
    return it == modified.end() ? 0 : it->second;
  }
  int64_t savedPage(const char* bookPath) override {
    const auto it = progress.find(bookPath);
    return it == progress.end() ? -1 : it->second;
  }
  void removeBook(const char* bookPath) override {
    log.push_back(std::string("removeBook ") + bookPath);
    files.erase(bookPath);
    progress.erase(bookPath);
  }
  void removeBookCache(const char* bookPath) override {
    log.push_back(std::string("removeCache ") + bookPath);
    progress.erase(bookPath);
  }
  uint64_t freeBytes() override { return free; }
  bool openForWrite(const char* path) override {
    log.push_back(std::string("open ") + path);
    if (failOpenSink) return false;
    files[path] = "";
    sinkPath = path;
    sinkOpen = true;
    return true;
  }
  bool write(const uint8_t* data, size_t size) override {
    if (failSinkWrite || !sinkOpen) return false;
    files[sinkPath].append(reinterpret_cast<const char*>(data), size);
    return true;
  }
  bool closeWrite() override {
    sinkOpen = false;
    return !failCloseSink;
  }
};

// ---- clock ----------------------------------------------------------------------------
struct FakeClock : method::ClockPort {
  uint32_t ms = 1000;
  int64_t unix = 1790000000;  // 2026-09-21 in UTC
  int offsetSeconds = 0;
  bool ready = false;
  uint32_t syncAfterMs = 300;
  bool syncNever = false;
  bool syncFails = false;
  bool syncStarted = false;
  uint32_t syncStartedAt = 0;
  int beginCalls = 0;
  int abortCalls = 0;
  int statusPolls = 0;
  // Models an adapter whose syncStatus() waits for the whole sync inside the call (as the first firmware adapter
  // did, through a blocking prepareClock()): the fake clock jumps to the end of the sync during the poll.
  bool blockingSync = false;

  uint32_t nowMs() override { return ms; }
  int64_t unixNow() override { return unix; }
  bool localTime(int64_t epoch, method::LocalTime& out) override {
    if (epoch <= 0) return false;
    out = localFromEpoch(epoch, offsetSeconds);
    return true;
  }
  uint32_t syncedAt = 0;
  bool clockReady() override { return ready && static_cast<uint32_t>(ms - syncedAt) < 90000; }
  void beginSync() override {
    ++beginCalls;
    syncStarted = true;
    syncStartedAt = ms;
  }
  method::SyncStatus syncStatus() override {
    ++statusPolls;
    if (!syncStarted || syncNever) return method::SyncStatus::Pending;
    if (syncFails) return method::SyncStatus::Failed;
    if (blockingSync && static_cast<uint32_t>(ms - syncStartedAt) < syncAfterMs) ms = syncStartedAt + syncAfterMs;
    if (static_cast<uint32_t>(ms - syncStartedAt) >= syncAfterMs) {
      ready = true;
      syncedAt = ms;
      return method::SyncStatus::Synced;
    }
    return method::SyncStatus::Pending;
  }
  void abortSync() override {
    if (syncStarted) ++abortCalls;
    syncStarted = false;
  }
  // The retained clock (message_tls::useRetainedClock()): trusted or not, as the test says. A trusted one grants the
  // same permission a finished sync does.
  bool retainedTrusted = false;
  int retainedCalls = 0;
  bool useRetained() override {
    ++retainedCalls;
    if (!retainedTrusted) return false;
    ready = true;
    syncedAt = ms;
    return true;
  }
};

// ---- Wi-Fi ----------------------------------------------------------------------------
struct FakeWifi : method::WifiPort {
  struct Network {
    uint32_t joinAfterMs = 800;
    bool failFast = false;  // the radio reports a failure at once
    bool visible = true;    // seen by the scan; an invisible network never joins
    int rssi = -60;
  };
  FakeClock& clock;
  std::vector<Network> saved;
  int last = -1;
  bool radioOn = false;
  bool joined = false;
  int connecting = -1;
  uint32_t connectStart = 0;
  bool scanning = false;
  uint32_t scanStart = 0;
  uint32_t scanTakesMs = 2000;
  bool scanFails = false;
  int offCalls = 0;
  int joinedIndex = -1;
  std::vector<std::string> log;

  explicit FakeWifi(FakeClock& clock) : clock(clock) {}

  bool connected() override { return joined; }
  size_t savedCount() override { return saved.size(); }
  int lastUsed() override { return last; }
  void beginConnect(size_t index) override {
    radioOn = true;
    connecting = static_cast<int>(index);
    connectStart = clock.ms;
    log.push_back("connect " + std::to_string(index));
  }
  method::WifiStatus connectStatus() override {
    if (connecting < 0) return method::WifiStatus::Failed;
    const Network& network = saved[static_cast<size_t>(connecting)];
    if (network.failFast) return method::WifiStatus::Failed;
    if (network.visible && static_cast<uint32_t>(clock.ms - connectStart) >= network.joinAfterMs) {
      joined = true;
      joinedIndex = connecting;
      connecting = -1;
      return method::WifiStatus::Connected;
    }
    return method::WifiStatus::Pending;
  }
  void abortConnect() override {
    if (connecting >= 0) log.push_back("abort " + std::to_string(connecting));
    connecting = -1;
  }
  void beginScan() override {
    radioOn = true;
    scanning = true;
    scanStart = clock.ms;
    log.push_back("scan");
  }
  method::ScanStatus scanStatus() override {
    if (scanFails) return method::ScanStatus::Failed;
    return static_cast<uint32_t>(clock.ms - scanStart) >= scanTakesMs ? method::ScanStatus::Done
                                                                      : method::ScanStatus::Pending;
  }
  std::vector<size_t> visibleSaved() override {
    std::vector<size_t> order;
    for (size_t i = 0; i < saved.size(); ++i)
      if (saved[i].visible) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return saved[a].rssi > saved[b].rssi; });
    return order;
  }
  void noteJoined(size_t index) override {
    last = static_cast<int>(index);
    log.push_back("joined " + std::to_string(index));
  }
  void off() override {
    ++offCalls;
    radioOn = false;
    joined = false;
    connecting = -1;
    scanning = false;
  }
};

// ---- HTTP -----------------------------------------------------------------------------
struct FakeHttp : method::HttpPort {
  struct Route {
    method::HttpOpen open = method::HttpOpen::Ok;
    std::string body;
    int64_t length = -2;        // -2: the body size
    uint32_t idleBefore = 0;    // Idle reads before the first byte
    size_t slice = 700;         // bytes per read
    size_t stallAt = SIZE_MAX;  // after this many bytes the server goes quiet
    bool failAtEnd = false;
  };
  std::map<std::string, Route> routes;
  bool heapReady = true;
  int readyWaits = 0;  // the next this-many ready() calls answer Wait (the render task holds the font caches)
  int readyCalls = 0;
  std::vector<std::string> opened;
  bool isOpen = false;
  int closeCalls = 0;
  Route current;
  size_t offset = 0;
  uint32_t idleLeft = 0;

  std::function<void(const std::string&)> onOpen;  // runs first on every open(): lets a test change the mailbox mid-run

  // The name lookup. resolvePolls: polls that answer Pending before Done; resolveNever: never answers; resolveFails.
  int resolvePolls = 0;
  bool resolveNever = false;
  bool resolveFails = false;
  bool resolving = false;  // a lookup is under way and has not been answered or abandoned
  int resolveBegins = 0;
  int resolveAborts = 0;
  int pollsSeen = 0;
  std::vector<std::string> sequence;  // "resolve" and "open <path>", in order
  void beginResolve() override {
    ++resolveBegins;
    resolving = true;
    pollsSeen = 0;
    sequence.push_back("resolve");
  }
  method::ResolveStatus resolveStatus() override {
    if (!resolving || resolveNever) return method::ResolveStatus::Pending;
    if (resolveFails) {
      resolving = false;
      return method::ResolveStatus::Failed;
    }
    if (pollsSeen++ < resolvePolls) return method::ResolveStatus::Pending;
    resolving = false;
    return method::ResolveStatus::Done;
  }
  void abortResolve() override {
    if (resolving) ++resolveAborts;
    resolving = false;
  }

  method::HttpReady ready() override {
    ++readyCalls;
    if (readyWaits != 0) {
      if (readyWaits > 0) --readyWaits;  // negative: wait for ever
      return method::HttpReady::Wait;
    }
    return heapReady ? method::HttpReady::Yes : method::HttpReady::NoMemory;
  }
  method::HttpOpen open(const char* path, size_t maxBytes) override {
    opened.push_back(path);
    sequence.push_back(std::string("open ") + path);
    const auto it = routes.find(path);
    const bool known = it != routes.end();
    if (known) current = it->second;  // what the server decided before the hook below changes the mailbox
    if (onOpen) onOpen(path);
    if (!known) return method::HttpOpen::Failed;
    if (current.open != method::HttpOpen::Ok) return current.open;
    if (static_cast<size_t>(contentLength()) > maxBytes) return method::HttpOpen::Failed;
    isOpen = true;
    offset = 0;
    idleLeft = current.idleBefore;
    return method::HttpOpen::Ok;
  }
  int64_t contentLength() override {
    return current.length == -2 ? static_cast<int64_t>(current.body.size()) : current.length;
  }
  method::HttpRead read(uint8_t* buffer, size_t capacity, size_t& got) override {
    got = 0;
    if (!isOpen) return method::HttpRead::Failed;
    if (idleLeft > 0) {
      --idleLeft;
      return method::HttpRead::Idle;
    }
    if (offset >= current.stallAt && offset < current.body.size()) return method::HttpRead::Idle;
    if (offset >= current.body.size()) return current.failAtEnd ? method::HttpRead::Failed : method::HttpRead::Done;
    size_t n = std::min({capacity, current.slice, current.body.size() - offset});
    if (current.stallAt != SIZE_MAX) n = std::min(n, current.stallAt - offset);
    memcpy(buffer, current.body.data() + offset, n);
    offset += n;
    got = n;
    return method::HttpRead::Data;
  }
  void close() override {
    if (isOpen) ++closeCalls;
    isOpen = false;
  }
};

struct FakeHash : method::HashPort {
  std::string data;
  bool active = false;
  bool failBegin = false;
  int abandonCalls = 0;
  bool begin() override {
    if (failBegin) return false;
    data.clear();
    active = true;
    return true;
  }
  bool update(const uint8_t* bytes, size_t size) override {
    data.append(reinterpret_cast<const char*>(bytes), size);
    return active;
  }
  bool finish(char out[65]) override {
    const std::string hex = sha256Hex(data);
    memcpy(out, hex.c_str(), 65);
    active = false;
    return true;
  }
  void abandon() override {
    active = false;
    ++abandonCalls;
  }
};

struct FakeBattery : method::BatteryPort {
  int level = 80;
  int percent() override { return level; }
};

struct FakeObserver : method::Observer {
  std::vector<std::string> events;
  std::map<std::string, uint32_t> slowestPass;  // per phase name, the value of the latest leave
  void phaseEntered(const char* name) override { events.push_back(std::string("enter ") + name); }
  void phaseLeft(const char* name, uint32_t elapsedMs, const char* outcome, uint32_t slowestPassMs) override {
    events.push_back(std::string("leave ") + name + " " + std::to_string(elapsedMs) + "ms " + outcome);
    slowestPass[name] = slowestPassMs;
  }
};

}  // namespace fake
