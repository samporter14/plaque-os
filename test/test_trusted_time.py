"""Compile TrustedTime against host stubs and check its timezone and sync contracts.

The stubs model SNTP closely enough for the sync loop: a sync completes a chosen number of milliseconds after
configTzTime() (re)starts it, reports COMPLETED once to whoever reads sntp_get_sync_status() first (the read consumes
it, as in esp_sntp.h), and runs the registered notification callback, which TrustedTime counts.
"""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    "Arduino.h": r'''
#pragma once
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <sys/time.h>
#include <atomic>
#include <cstdio>
#include <cstring>
using portMUX_TYPE = int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
// The system clock, as TrustedTime reads and sets it: time() and settimeofday() are the stubs below, so a test says what
// the clock reads (an RC clock running fast, a spoofed SNTP answer) and sees when TrustedTime puts it back.
inline time_t fakeClock = ::time(nullptr);
inline int clockSets = 0;
inline time_t fakeTime(time_t* out) {
  if (out) *out = fakeClock;
  return fakeClock;
}
inline int fakeSetTimeOfDay(const timeval* tv, const void*) {
  fakeClock = tv->tv_sec;
  ++clockSets;
  return 0;
}
#define time(arg) fakeTime(arg)
#define settimeofday(tv, tz) fakeSetTimeOfDay(tv, tz)
inline bool enabled = false;
inline int configurations = 0;
inline unsigned long ticks = 0;
inline int errLogs = 0;
inline int statusReads = 0;

// The SNTP model. completeAfter: milliseconds after the latest configTzTime() at which the sync completes, or -1 for
// never. A completed sync reports COMPLETED once (reportStatus) and runs the notification callback (fireCallback).
inline long completeAfter = 0;
inline bool reportStatus = true;
inline bool fireCallback = true;
inline unsigned long modelStart = 0;
inline bool modelFired = false;
inline bool statusPending = false;
inline int callbackFires = 0;
inline void (*syncCallback)(timeval*) = nullptr;
inline void modelStep() {
  if (!enabled || completeAfter < 0 || modelFired || ticks - modelStart < static_cast<unsigned long>(completeAfter)) return;
  modelFired = true;
  if (reportStatus) statusPending = true;
  if (fireCallback && syncCallback) {
    ++callbackFires;
    syncCallback(nullptr);
  }
}
inline unsigned long millis() { return ticks; }
inline void delay(unsigned long ms) {
  ticks += ms;
  modelStep();
}
// Restarting SNTP is not instant (it stops and re-creates the client), so the order of beginSync()'s steps against
// the caller's clock reads is observable.
inline constexpr unsigned long restartMs = 7;
// The servers the latest configTzTime() named, as lwIP would hold them (the pointers, which must stay valid).
inline const char* configuredServer[3] = {nullptr, nullptr, nullptr};
inline void configTzTime(const char* tz, const char* server1, const char* server2 = nullptr,
                         const char* server3 = nullptr) {
  configuredServer[0] = server1;
  configuredServer[1] = server2;
  configuredServer[2] = server3;
  enabled = true;
  ++configurations;
  ticks += restartMs;
  modelStart = ticks;
  modelFired = false;
  statusPending = false;
  setenv("TZ", tz, 1);
  tzset();
}
''',
    # NVS as a plain variable, so a test can see that the floor was persisted.
    "Preferences.h": r'''
#pragma once
#include <cstdint>
inline int64_t storedFloor = 0;
inline int floorWrites = 0;
struct Preferences {
  bool begin(const char*, bool) { return true; }
  int64_t getLong64(const char*, int64_t) { return storedFloor; }
  void putLong64(const char*, int64_t value) {
    storedFloor = value;
    ++floorWrites;
  }
  void end() {}
};
''',
    "Logging.h": '#pragma once\n#include <Arduino.h>\n#define LOG_DBG(...) ((void)0)\n#define LOG_ERR(...) (++errLogs)\n',
    "esp_sntp.h": r'''
#pragma once
#include <Arduino.h>
constexpr int SNTP_SYNC_STATUS_COMPLETED = 1;
inline bool esp_sntp_enabled() { return enabled; }
// Reading COMPLETED resets it, exactly as esp_sntp.h documents.
inline int sntp_get_sync_status() {
  ++statusReads;
  modelStep();
  if (!statusPending) return 0;
  statusPending = false;
  return SNTP_SYNC_STATUS_COMPLETED;
}
inline void sntp_set_time_sync_notification_cb(void (*callback)(timeval*)) { syncCallback = callback; }

// lwIP's server slots, as far as TrustedTime looks at them: a name or a bare address per slot. configTzTime() above
// records the names it is given; a DHCP lease (leaseAddress) writes a bare address into slot 0, as lwIP does.
#define LWIP_DHCP_GET_NTP_SRV 1
#define LWIP_DHCP_MAX_NTP_SERVERS 1
using u8_t = unsigned char;
struct ip_addr_t {
  unsigned addr = 0;
};
#define ip_addr_isany(a) ((a) == nullptr || (a)->addr == 0)
inline ip_addr_t slotAddress[3];
inline int clearedSlots = 0;
inline int clearCalls = 0;
inline int stops = 0;
inline int dhcpModeCalls = 0;
inline bool dhcpMode = false;
inline const char* esp_sntp_getservername(u8_t idx) { return configuredServer[idx]; }
inline const ip_addr_t* esp_sntp_getserver(u8_t idx) { return &slotAddress[idx]; }
inline void esp_sntp_setserver(u8_t idx, const ip_addr_t* addr) {
  ++clearCalls;
  if (!addr) clearedSlots |= 1 << idx;
  slotAddress[idx] = addr ? *addr : ip_addr_t{};
  configuredServer[idx] = nullptr;
}
inline void esp_sntp_servermode_dhcp(bool enable) {
  ++dhcpModeCalls;
  dhcpMode = enable;
}
inline void esp_sntp_stop() {
  ++stops;
  enabled = false;
}
inline void leaseAddress(unsigned address) {
  slotAddress[0].addr = address;
  configuredServer[0] = nullptr;
}
''',
}

CHECK = r'''
#include <TrustedTime.h>
#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_sntp.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace {
int checks = 0;
void expect(bool condition, const char* what, int line) {
  if (!condition) {
    std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
    std::exit(1);
  }
  ++checks;
}
#define CHECK(condition) expect((condition), #condition, __LINE__)

// What a caller can observe of one sync attempt.
struct Outcome {
  bool result;
  unsigned long elapsed;
  int statusReads;
  int configurations;
  int errLogs;
  int64_t floorWritten;
  bool operator==(const Outcome& o) const {
    return result == o.result && elapsed == o.elapsed && statusReads == o.statusReads &&
           configurations == o.configurations && errLogs == o.errLogs && floorWritten == o.floorWritten;
  }
};

void reset(long after, bool status, bool callback) {
  completeAfter = after;
  reportStatus = status;
  fireCallback = callback;
  statusPending = false;
  modelFired = false;
  modelStart = ticks;
  enabled = false;
  storedFloor = 0;
  configurations = 0;
  statusReads = 0;
  errLogs = 0;
}

// syncNow() as it was before beginSync() and syncCompleted() were split out of it (commit 465834e), written
// against the same public pieces: the sync count is the number of callbacks that ran.
bool referenceSyncNow(uint32_t timeoutMs) {
  const int before = callbackFires;
  configTzTime("UTC0", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  const unsigned long started = millis();
  const unsigned long deadline = started + timeoutMs;
  bool synced = false;
  while (static_cast<long>(deadline - millis()) > 0) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED || callbackFires != before) {
      synced = true;
      break;
    }
    delay(100);
  }
  if (synced) {
    trustedtime::note();
  } else {
    LOG_ERR("TIME", "SNTP sync timed out after %lu ms", millis() - started);
  }
  return synced;
}

template <typename Sync>
Outcome observe(long after, bool status, bool callback, Sync sync, uint32_t timeoutMs) {
  reset(after, status, callback);
  const unsigned long t0 = ticks;
  const bool result = sync(timeoutMs);
  return {result, ticks - t0, statusReads, configurations, errLogs, storedFloor};
}
}  // namespace

int main() {
  trustedtime::init();  // registers the notification callback, as setup() does

  // --- Timezone: SNTP never changes the display zone; startSync() only configures once ---
  const char* zone = "EST5EDT,M3.2.0,M11.1.0";
  setenv("TZ", zone, 1);
  tzset();
  reset(0, true, true);
  trustedtime::startSync();
  CHECK(configurations == 1 && strcmp(getenv("TZ"), zone) == 0);
  trustedtime::startSync();
  CHECK(configurations == 1);
  CHECK(trustedtime::syncNow(200));
  CHECK(configurations == 2 && strcmp(getenv("TZ"), zone) == 0);
  completeAfter = -1;
  CHECK(!trustedtime::syncNow(200));
  CHECK(strcmp(getenv("TZ"), zone) == 0);
  unsetenv("TZ");
  enabled = false;
  trustedtime::startSync();
  CHECK(strcmp(getenv("TZ"), "UTC0") == 0);
  setenv("TZ", zone, 1);
  tzset();

  // --- syncNow() still does exactly what it did before it was built on beginSync() and syncCompleted() ---
  // Every completion time around the 100 ms poll boundaries and the deadline, with the completion reported by the
  // status, by the callback only (another reader consumed the status), by both, or never reported; same result, same
  // time spent, same status reads, same SNTP restarts, same error log, same persisted floor.
  {
    const long completions[] = {0, 1, 99, 100, 101, 199, 200, 250, 999, 1000, 4999, 5000, 5001, 19999, 20000, 20001, -1};
    // Deadlines on and just past a poll (the 7 ms SNTP restart sits between them), so the order of beginSync()
    // against the first clock read matters.
    const uint32_t timeouts[] = {0, 3, 50, 53, 200, 203, 1000, 1003, 5000, 5003, 20000, 20003};
    struct Mode {
      bool status, callback;
    };
    const Mode modes[] = {{true, true}, {false, true}, {true, false}, {false, false}};
    int compared = 0;
    int succeeded = 0;
    int timedOut = 0;
    for (const Mode mode : modes) {
      for (const long after : completions) {
        for (const uint32_t timeout : timeouts) {
          const Outcome before = observe(after, mode.status, mode.callback, referenceSyncNow, timeout);
          const Outcome now = observe(after, mode.status, mode.callback, trustedtime::syncNow, timeout);
          CHECK(before == now);
          ++compared;
          (now.result ? succeeded : timedOut)++;
        }
      }
    }
    CHECK(compared == 4 * 17 * 12);
    CHECK(succeeded > 100 && timedOut > 100);  // the grid exercises both ends
    setenv("TZ", zone, 1);                     // the reference sets UTC through configTzTime; put the display zone back
    tzset();
  }

  // --- Success persists the floor and logs nothing; a timeout logs once and persists nothing ---
  {
    const Outcome ok = observe(300, true, true, trustedtime::syncNow, 20000);
    CHECK(ok.result && ok.errLogs == 0 && ok.floorWritten > 0 && ok.configurations == 1);
    CHECK(ok.elapsed == restartMs + 300);  // the 100 ms poll sees it on the first pass at or after 300 ms
    const Outcome late = observe(-1, true, true, trustedtime::syncNow, 20000);
    CHECK(!late.result && late.errLogs == 1 && late.floorWritten == 0 && late.elapsed == restartMs + 20000);
  }

  // --- beginSync() / syncCompleted(): the same sync, polled by the caller ---
  {
    reset(2500, true, true);
    const int before = callbackFires;
    const uint32_t token = trustedtime::beginSync();
    CHECK(configurations == 1 && strcmp(getenv("TZ"), zone) == 0);  // restarted SNTP once, zone untouched
    CHECK(statusReads == 0);                                          // beginSync() waits for nothing
    CHECK(storedFloor == 0);

    int polls = 0;
    bool done = false;
    unsigned long finishedAt = 0;
    const unsigned long begun = ticks;
    for (; ticks - begun < 5000 && !done; ++polls) {
      const int readsBefore = statusReads;
      done = trustedtime::syncCompleted(token);
      CHECK(statusReads == readsBefore + 1);  // one status read per call: the read consumes COMPLETED
      if (done) {
        finishedAt = ticks - begun;
      } else {
        delay(10);  // the shelf's loop pass
      }
    }
    CHECK(done && finishedAt == 2500);  // seen on the first poll at or after completion: no 100 ms granularity
    CHECK(polls == 251);
    CHECK(callbackFires == before + 1 && storedFloor > 0);  // the floor was persisted, as syncNow() does
    CHECK(configurations == 1);                              // polling never restarts the sync
    CHECK(errLogs == 0);

    // The callback keeps the answer stable even though the status was consumed by the poll that saw it.
    CHECK(trustedtime::syncCompleted(token));

    // A new begin forgets the finished sync: its callback no longer counts, and the new one does.
    const uint32_t second = trustedtime::beginSync();
    CHECK(second == token + 1 && configurations == 2);
    CHECK(!trustedtime::syncCompleted(second));
    delay(2500);
    CHECK(trustedtime::syncCompleted(second));
  }

  // --- Completion seen only through the callback (another reader consumed the status) still counts ---
  {
    reset(100, false, true);
    const uint32_t token = trustedtime::beginSync();
    CHECK(!trustedtime::syncCompleted(token));
    delay(100);
    CHECK(trustedtime::syncCompleted(token));
    CHECK(storedFloor > 0);
  }

  // --- Completion seen only through the status (no callback) is reported by the first poll that reads it ---
  {
    reset(100, true, false);
    const uint32_t token = trustedtime::beginSync();
    delay(100);
    CHECK(trustedtime::syncCompleted(token));
    // The read consumed the status and no callback ran, so a second poll cannot see it again: callers latch the
    // first answer (the Method shelf's DeviceClock does).
    CHECK(!trustedtime::syncCompleted(token));
  }

  // --- A sync that never completes is just never reported: the caller owns the deadline, and abandoning it is free ---
  {
    reset(-1, true, true);
    const uint32_t token = trustedtime::beginSync();
    for (int i = 0; i < 3000; ++i) {
      CHECK(!trustedtime::syncCompleted(token));
      delay(10);
    }
    CHECK(configurations == 1 && errLogs == 0 && storedFloor == 0);
  }

  // --- Time servers: nobody configured anything, so every sync asks exactly what it always asked ---
  {
    reset(0, true, true);
    clearCalls = 0;
    dhcpModeCalls = 0;
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "pool.ntp.org") == 0);
    CHECK(strcmp(configuredServer[1], "time.google.com") == 0);
    CHECK(strcmp(configuredServer[2], "time.cloudflare.com") == 0);
    CHECK(clearCalls == 0 && dhcpModeCalls == 0);  // no lwIP call beyond configTzTime(), as before
    CHECK(strcmp(getenv("TZ"), zone) == 0);
  }

  // --- Configured servers replace the defaults, in order; unused slots are cleared, not left with an old address ---
  {
    reset(0, true, true);
    char first[32] = "ntp1.example.edu";
    char second[32] = "10.0.0.53";
    const char* both[] = {first, second};
    CHECK(trustedtime::setServers(both, 2));
    first[0] = 'X';  // the caller's buffer may change or go away: TrustedTime keeps its own copy
    second[0] = 'X';
    clearCalls = 0;
    clearedSlots = 0;
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "ntp1.example.edu") == 0);
    CHECK(strcmp(configuredServer[1], "10.0.0.53") == 0);
    CHECK(configuredServer[2] == nullptr && clearedSlots == 4);
    CHECK(configuredServer[0] != first && configuredServer[1] != second);

    const char* one[] = {"time.example.edu"};
    CHECK(trustedtime::setServers(one, 1));
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "time.example.edu") == 0);
    CHECK(configuredServer[1] == nullptr && configuredServer[2] == nullptr);

    // Refused, and nothing changes: more than three, an empty name, a space, a control character, too long.
    const char* four[] = {"a.example", "b.example", "c.example", "d.example"};
    CHECK(!trustedtime::setServers(four, 4));
    const char* empty[] = {""};
    CHECK(!trustedtime::setServers(empty, 1));
    const char* spaced[] = {"time example.edu"};
    CHECK(!trustedtime::setServers(spaced, 1));
    const char* control[] = {"time.example.edu\n"};
    CHECK(!trustedtime::setServers(control, 1));
    const char* none[] = {nullptr};
    CHECK(!trustedtime::setServers(none, 1));
    static char longName[300];
    memset(longName, 'a', 254);
    longName[254] = '\0';
    const char* tooLong[] = {longName};
    CHECK(!trustedtime::setServers(tooLong, 1));
    longName[253] = '\0';  // exactly 253 characters is a valid host name length
    CHECK(trustedtime::setServers(tooLong, 1));
    CHECK(trustedtime::setServers(one, 1));
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "time.example.edu") == 0 && configuredServer[1] == nullptr);

    // Three is the most lwIP holds here (CONFIG_LWIP_SNTP_MAX_SERVERS).
    const char* three[] = {"a.example", "b.example", "c.example"};
    CHECK(trustedtime::setServers(three, 3));
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[2], "c.example") == 0);

    // Changing the texts while a client runs stops it first, so lwIP never reads a half-written name.
    enabled = true;
    stops = 0;
    CHECK(trustedtime::setServers(one, 1));
    CHECK(stops == 1 && !enabled);

    // Back to the defaults: exactly today's three.
    CHECK(trustedtime::setServers(nullptr, 0));
    clearCalls = 0;
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "pool.ntp.org") == 0 && strcmp(configuredServer[1], "time.google.com") == 0 &&
          strcmp(configuredServer[2], "time.cloudflare.com") == 0 && clearCalls == 0);
  }

  // --- DHCP-offered servers: asked first when on, ignored when off ---
  {
    reset(0, true, true);
    dhcpModeCalls = 0;
    trustedtime::setUseDhcpServers(false);  // already off: lwIP is not called
    CHECK(dhcpModeCalls == 0);
    trustedtime::setUseDhcpServers(true);
    CHECK(dhcpModeCalls == 1 && dhcpMode);
    trustedtime::setUseDhcpServers(true);
    CHECK(dhcpModeCalls == 1);

    // No lease server yet: the defaults, all three.
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "pool.ntp.org") == 0 && strcmp(configuredServer[2], "time.cloudflare.com") == 0);

    // The lease put its server in slot 0: it stays, the defaults follow it.
    leaseAddress(0x0a000035);
    clearedSlots = 0;
    trustedtime::beginSync();
    CHECK(configuredServer[0] == nullptr && slotAddress[0].addr == 0x0a000035);
    CHECK(strcmp(configuredServer[1], "pool.ntp.org") == 0 && strcmp(configuredServer[2], "time.google.com") == 0);
    CHECK(clearedSlots == 0);
    // A later sync in the same session still finds it.
    trustedtime::beginSync();
    CHECK(configuredServer[0] == nullptr && slotAddress[0].addr == 0x0a000035 &&
          strcmp(configuredServer[1], "pool.ntp.org") == 0);

    // With configured servers: the lease's server, then the configured ones, three in all.
    const char* three[] = {"a.example", "b.example", "c.example"};
    CHECK(trustedtime::setServers(three, 3));
    trustedtime::beginSync();
    CHECK(configuredServer[0] == nullptr && strcmp(configuredServer[1], "a.example") == 0 &&
          strcmp(configuredServer[2], "b.example") == 0);

    // Off again: the lease's address is not asked, and slot 0 gets a name.
    trustedtime::setUseDhcpServers(false);
    CHECK(dhcpModeCalls == 2 && !dhcpMode);
    trustedtime::beginSync();
    CHECK(strcmp(configuredServer[0], "a.example") == 0 && strcmp(configuredServer[2], "c.example") == 0);
    CHECK(trustedtime::setServers(nullptr, 0));
  }


  // --- The floor policy: the system clock never reaches NVS; only what something else vouches for does ---
  {
    using trustedtime::MAX_VALID_EPOCH;
    using trustedtime::MIN_VALID_EPOCH;
    const int64_t T0 = static_cast<int64_t>(::time(nullptr)) > MIN_VALID_EPOCH + 1000000
                           ? static_cast<int64_t>(::time(nullptr))
                           : MIN_VALID_EPOCH + 1000000;
    constexpr int64_t DAY = 86400;
    static int64_t vouched = 0;
    static int vouchedCalls = 0;  // the chip is read on the main task only: never from the sync callback
    static const trustedtime::FloorPolicy policy = {[]() -> int64_t {
      ++vouchedCalls;
      return vouched;
    }};

    // The failure this replaces: without a policy a forward-spoofed SNTP answer is written to the floor, and kept.
    trustedtime::setFloorPolicy(nullptr);
    trustedtime::resetFloor();
    fakeClock = T0 + DAY;
    reset(0, true, true);
    CHECK(trustedtime::syncNow(200));
    CHECK(storedFloor == T0 + DAY);  // legacy: the lie is persisted

    // Beyond 2100 is never persisted, with or without a policy.
    trustedtime::resetFloor();
    fakeClock = MAX_VALID_EPOCH + 3600;
    trustedtime::note();
    CHECK(storedFloor == 0 && trustedtime::trustedNow() == 0);
    fakeClock = MAX_VALID_EPOCH;
    trustedtime::note();
    CHECK(storedFloor == 0 && trustedtime::trustedNow() == 0);
    fakeClock = MAX_VALID_EPOCH - 1;
    CHECK(trustedtime::trustedNow() == MAX_VALID_EPOCH - 1);  // the last second before 2100 is a time
    trustedtime::resetFloor();

    // With the policy installed: +1 day, +90 days and beyond 2100, through the sync callback, note() and a sync loop.
    trustedtime::setFloorPolicy(&policy);
    for (const int64_t spoof : {DAY, 90 * DAY, MAX_VALID_EPOCH + 10 * 365 * DAY - T0}) {
      trustedtime::resetFloor();
      vouched = 0;
      reset(0, true, true);
      fakeClock = T0 + spoof;
      vouchedCalls = 0;
      CHECK(trustedtime::syncNow(200));  // the sync callback ran
      CHECK(callbackFires > 0 && vouchedCalls == 0);
      trustedtime::note();
      CHECK(vouchedCalls == 1);
      CHECK(storedFloor == 0);
      CHECK(trustedtime::provenFloor() == 0);
      if (T0 + spoof < MAX_VALID_EPOCH) {
        CHECK(trustedtime::trustedNow() == T0 + spoof);  // this boot still sees the clock it was given...
        fakeClock = T0;                                  // ...and when SNTP corrects it the monotonic floor holds,
        CHECK(trustedtime::trustedNow() == T0 + spoof);
        CHECK(trustedtime::provenFloor() == 0);          // ...but the proven floor never learned it
      } else {
        CHECK(trustedtime::trustedNow() == 0);
      }
    }

    // A vouched time raises the proven floor and is persisted; the clock does not matter.
    trustedtime::resetFloor();
    fakeClock = T0 + 7 * DAY;  // an RC clock that ran fast through a sleep
    vouched = T0;
    floorWrites = 0;
    trustedtime::note();
    CHECK(trustedtime::provenFloor() == T0 && storedFloor == T0 && floorWrites == 1);
    vouched = T0 + 30;  // under a minute on: remembered in RAM, not rewritten to NVS
    trustedtime::note();
    CHECK(trustedtime::provenFloor() == T0 + 30 && storedFloor == T0 && floorWrites == 1);
    vouched = T0 + 120;
    trustedtime::note();
    CHECK(storedFloor == T0 + 120 && floorWrites == 2);
    vouched = T0 + 60;  // never down
    trustedtime::note();
    CHECK(trustedtime::provenFloor() == T0 + 120 && storedFloor == T0 + 120);
    vouched = 0;
    trustedtime::note();
    CHECK(trustedtime::provenFloor() == T0 + 120);

    // raiseProvenFloor(): up only, plausible only.
    floorWrites = 0;
    CHECK(!trustedtime::raiseProvenFloor(T0 + 100));
    CHECK(!trustedtime::raiseProvenFloor(0) && !trustedtime::raiseProvenFloor(-5) &&
          !trustedtime::raiseProvenFloor(MIN_VALID_EPOCH - 1) && !trustedtime::raiseProvenFloor(MAX_VALID_EPOCH) &&
          !trustedtime::raiseProvenFloor(MAX_VALID_EPOCH + 100 * 365 * DAY));
    CHECK(trustedtime::provenFloor() == T0 + 120 && floorWrites == 0);
    CHECK(trustedtime::raiseProvenFloor(T0 + 120 + DAY));
    CHECK(trustedtime::provenFloor() == T0 + 120 + DAY && storedFloor == T0 + 120 + DAY);
    fakeClock = T0;
    CHECK(trustedtime::trustedNow() == T0 + 120 + DAY);  // and the monotonic floor follows it, whatever the clock reads

    // lowerProvenFloor(): the one way down, for the first sync after an older firmware left the floor ahead.
    trustedtime::resetFloor();
    vouched = 0;
    CHECK(trustedtime::raiseProvenFloor(T0 + 3600) && storedFloor == T0 + 3600);
    floorWrites = 0;
    CHECK(!trustedtime::lowerProvenFloor(T0 + 3600) && !trustedtime::lowerProvenFloor(T0 + 7200));  // not below: no
    CHECK(!trustedtime::lowerProvenFloor(0) && !trustedtime::lowerProvenFloor(MIN_VALID_EPOCH - 1));  // not a time: no
    CHECK(trustedtime::provenFloor() == T0 + 3600 && floorWrites == 0);
    fakeClock = T0 + 3600;
    CHECK(trustedtime::lowerProvenFloor(T0));
    CHECK(trustedtime::provenFloor() == T0 && storedFloor == T0 && floorWrites == 1);
    fakeClock = T0 + 10;
    CHECK(trustedtime::trustedNow() == T0 + 10);  // the monotonic floor came down with it: not an hour ahead
    fakeClock = T0 - 5;
    CHECK(trustedtime::trustedNow() == T0 + 10);  // and it holds from there as before
    trustedtime::resetFloor();

    // Boot: the persisted floor is the proven floor, and the clock is put back to it when it restarted below it.
    trustedtime::resetFloor();
    storedFloor = T0 + 5 * DAY;
    fakeClock = 1000;  // a cold boot
    clockSets = 0;
    trustedtime::init();
    CHECK(trustedtime::provenFloor() == T0 + 5 * DAY && fakeClock == T0 + 5 * DAY && clockSets == 1);
    storedFloor = MAX_VALID_EPOCH + DAY;  // a persisted floor beyond 2100 is not one
    trustedtime::resetFloor();
    clockSets = 0;
    fakeClock = 1000;
    trustedtime::init();
    CHECK(trustedtime::provenFloor() == 0 && clockSets == 0 && fakeClock == 1000);

    // "Reset the clock check": the persisted and RAM floors go, and a system clock too far ahead is put where no time
    // is believed. It is logged where the serial log keeps it (an error).
    trustedtime::resetFloor();
    vouched = T0 + 40 * DAY;
    trustedtime::note();
    fakeClock = T0 + 40 * DAY;
    CHECK(trustedtime::trustedNow() == T0 + 40 * DAY && storedFloor == T0 + 40 * DAY);
    errLogs = 0;
    trustedtime::resetFloor();
    CHECK(storedFloor == 0 && trustedtime::provenFloor() == 0);
    // Zero, not MIN_VALID_EPOCH: that second is the first plausible() accepts, so the reader would read as trusted.
    CHECK(fakeClock == 0 && trustedtime::trustedNow() == 0);
    fakeClock += 5;  // and it stays so as the clock ticks
    CHECK(trustedtime::trustedNow() == 0);
    fakeClock = 0;
    CHECK(errLogs == 1);
    vouched = 0;
    // After a reset the next honest answer is not behind anything.
    fakeClock = T0;
    CHECK(trustedtime::trustedNow() == T0);
    trustedtime::resetFloor();
    trustedtime::setFloorPolicy(nullptr);
  }

  // --- The prepare hook: whoever starts a sync gets the profile's servers, because TrustedTime asks for them ---
  {
    static int hookCalls = 0;
    static int configurationsAtHook = -1;
    static const char* profileServer = "ntp.profile.example";
    trustedtime::setSyncPrepareHook([]() {
      ++hookCalls;
      configurationsAtHook = configurations;
      const char* names[] = {profileServer};
      trustedtime::setServers(names, 1);
    });

    // startSync() starts SNTP once; the hook runs before it configures, not when it is already running.
    reset(0, true, true);
    CHECK(trustedtime::setServers(nullptr, 0));
    hookCalls = 0;
    enabled = false;
    trustedtime::startSync();
    CHECK(hookCalls == 1 && configurationsAtHook == 0 && configurations == 1);
    CHECK(strcmp(configuredServer[0], "ntp.profile.example") == 0 && configuredServer[1] == nullptr);
    trustedtime::startSync();  // running: nothing to configure, nothing to prepare
    CHECK(hookCalls == 1 && configurations == 1);

    // beginSync() and syncNow() prepare every time, and the servers of the profile are what SNTP is asked.
    profileServer = "ntp2.profile.example";
    trustedtime::beginSync();
    CHECK(hookCalls == 2 && strcmp(configuredServer[0], "ntp2.profile.example") == 0);
    profileServer = "ntp3.profile.example";
    reset(0, true, true);
    hookCalls = 0;
    CHECK(trustedtime::syncNow(200));
    CHECK(hookCalls == 1 && strcmp(configuredServer[0], "ntp3.profile.example") == 0 && configuredServer[1] == nullptr);

    // The same list again is not a change: a running client is not stopped for it.
    enabled = true;
    stops = 0;
    const char* same[] = {"ntp3.profile.example"};
    CHECK(trustedtime::setServers(same, 1));
    CHECK(stops == 0 && enabled);
    const char* other[] = {"ntp4.profile.example"};
    CHECK(trustedtime::setServers(other, 1));
    CHECK(stops == 1 && !enabled);
    const char* two[] = {"ntp4.profile.example", "b.example"};
    CHECK(trustedtime::setServers(two, 2));  // a longer list with the same first name is a change
    enabled = true;
    stops = 0;
    CHECK(trustedtime::setServers(two, 2) && stops == 0);
    CHECK(trustedtime::setServers(other, 1) && stops == 1);

    trustedtime::setSyncPrepareHook(nullptr);
    hookCalls = 0;
    trustedtime::beginSync();
    CHECK(hookCalls == 0);
    CHECK(trustedtime::setServers(nullptr, 0));
  }

  std::printf("%d TrustedTime checks passed\n", checks);
}
'''


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="trusted-time-test-") as directory:
        work = Path(directory)
        for name, content in STUBS.items():
            (work / name).write_text(content)
        (work / "check.cpp").write_text(CHECK)
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-I{work}", f"-I{ROOT / 'lib/TrustedTime'}",
            str(ROOT / "lib/TrustedTime/TrustedTime.cpp"), str(ROOT / "lib/TrustedTime/TimeServers.cpp"),
            str(work / "check.cpp"),
            "-o", str(work / "check"),
        ], check=True)
        subprocess.run([str(work / "check")], check=True)
    print("TrustedTime timezone and sync checks passed")
