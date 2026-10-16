#include "TrustedTime.h"

#include <Arduino.h>
#include <Logging.h>
#include <Preferences.h>
#include <esp_sntp.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "TimeServers.h"

namespace trustedtime {

namespace {

// MIN_VALID_EPOCH and MAX_VALID_EPOCH are in TrustedTime.h: the clock was obviously never set below the first, and a
// time above the second is not one anybody has seen.
bool plausible(const int64_t value) { return value >= MIN_VALID_EPOCH && value < MAX_VALID_EPOCH; }
// NVS wear guard: only rewrite the floor when it moved by at least this much.
constexpr int64_t MIN_ADVANCE_SECS = 60;

constexpr const char* PREFS_NAMESPACE = "cptime";
constexpr const char* PREFS_KEY = "floor";

// Latest time this boot has seen (or restored from NVS). trustedNow() never
// reports earlier, so a backward clock step (a bad SNTP answer, a manual set)
// cannot reopen an expired loan. Updated from the SNTP callback's lwIP task as
// well as the main task; 64-bit atomics are not lock-free on the C3.
int64_t ramFloor = 0;
// The proven floor (see FloorPolicy): the persisted floor and what accepted syncs raised it to. Under the same lock.
int64_t provenFloorValue = 0;
const FloorPolicy* floorPolicy = nullptr;
void (*prepareHook)() = nullptr;
portMUX_TYPE ramFloorLock = portMUX_INITIALIZER_UNLOCKED;

// Raises the in-RAM floor to `value` if higher; returns the resulting floor.
int64_t raiseFloor(const int64_t value) {
  portENTER_CRITICAL(&ramFloorLock);
  if (value > ramFloor) ramFloor = value;
  const int64_t floor = ramFloor;
  portEXIT_CRITICAL(&ramFloorLock);
  return floor;
}

int64_t readFloor() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true)) return 0;
  const int64_t value = prefs.getLong64(PREFS_KEY, 0);
  prefs.end();
  return value;
}

void writeFloor(const int64_t value) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false)) return;
  prefs.putLong64(PREFS_KEY, value);
  prefs.end();
}

void runPrepareHook() {
  if (prepareHook) prepareHook();
}

// SNTP sync callback (lwIP task context; Preferences/NVS is mutex-guarded).
// Counts SNTP time sets, so a waiter can see a completed sync even if some
// other reader of sntp_get_sync_status() consumed the COMPLETED status first.
std::atomic<uint32_t> syncCount{0};
void onTimeSynced(struct timeval*) {
  syncCount.fetch_add(1, std::memory_order_acq_rel);
  // With a floor policy the clock the sync just set is judged by whoever records the sync (on the main task, with the
  // RTC chip to hand), not written to NVS from here.
  if (!floorPolicy) note();
}

// The servers set with setServers(), copied so lwIP can keep pointing at them
// (sntp_setservername stores the pointer, not the text). Main task only.
char configuredServers[MAX_SERVERS][MAX_SERVER_CHARS + 1] = {};
const char* configuredNames[MAX_SERVERS] = {};
size_t configuredCount = 0;
bool useDhcpServers = false;

// How many leading slots hold a DHCP lease's server: lwIP writes a lease's
// servers as bare addresses (no name) from slot 0. Every other way a slot is
// filled here sets a name, or clears it.
size_t leaseSlots() {
  if (!useDhcpServers) return 0;
  size_t count = 0;
#if LWIP_DHCP_GET_NTP_SRV
  for (; count < MAX_SERVERS && count < LWIP_DHCP_MAX_NTP_SERVERS; ++count) {
    const ip_addr_t* address = esp_sntp_getserver(static_cast<u8_t>(count));
    if (esp_sntp_getservername(static_cast<u8_t>(count)) != nullptr || address == nullptr || ip_addr_isany(address))
      break;
  }
#endif
  return count;
}

void configureSntp() {
  // SNTP uses UTC epochs regardless of the display timezone. Copy TZ before
  // configTzTime replaces the environment entry, as HalClock::syncFromNTP does.
  const char* currentTz = getenv("TZ");
  char timezone[64];
  snprintf(timezone, sizeof(timezone), "%s", currentTz ? currentTz : "UTC0");
  const ServerPlan plan = planServers(configuredNames, configuredCount, leaseSlots());
  // A slot that ends up unnamed must not keep an address an earlier name
  // resolved to (configTzTime only replaces names). The defaults name all three.
  for (size_t slot = 0; slot < MAX_SERVERS; ++slot)
    if (plan.use[slot] == SlotUse::Clear) esp_sntp_setserver(static_cast<u8_t>(slot), nullptr);
  configTzTime(timezone, plan.name[0], plan.name[1], plan.name[2]);
}

}  // namespace

void init() {
  sntp_set_time_sync_notification_cb(&onTimeSynced);
  const int64_t floor = readFloor();
  if (!plausible(floor)) return;
  raiseFloor(floor);
  portENTER_CRITICAL(&ramFloorLock);
  if (floor > provenFloorValue) provenFloorValue = floor;
  portEXIT_CRITICAL(&ramFloorLock);
  if (static_cast<int64_t>(time(nullptr)) < floor) {
    // Cold boot reset the clock; resume from the floor so time keeps moving
    // forward across power cycles instead of restarting at epoch 0.
    timeval tv = {static_cast<time_t>(floor), 0};
    settimeofday(&tv, nullptr);
    LOG_DBG("TIME", "Clock restored to persisted floor");
  }
}

void note() {
  if (floorPolicy) {
    // Not the clock: what something else vouches for (the RTC chip), if anything does.
    const int64_t vouched = floorPolicy->vouchedNow ? floorPolicy->vouchedNow() : 0;
    if (vouched) raiseProvenFloor(vouched);
    return;
  }
  const int64_t now = static_cast<int64_t>(time(nullptr));
  if (!plausible(now)) return;
  raiseFloor(now);
  if (now - readFloor() >= MIN_ADVANCE_SECS) writeFloor(now);
}

void setFloorPolicy(const FloorPolicy* policy) { floorPolicy = policy; }

int64_t provenFloor() {
  portENTER_CRITICAL(&ramFloorLock);
  const int64_t value = provenFloorValue;
  portEXIT_CRITICAL(&ramFloorLock);
  return value;
}

bool raiseProvenFloor(const int64_t epoch) {
  if (!plausible(epoch)) return false;
  bool moved = false;
  portENTER_CRITICAL(&ramFloorLock);
  if (epoch > provenFloorValue) {
    provenFloorValue = epoch;
    moved = true;
  }
  portEXIT_CRITICAL(&ramFloorLock);
  if (!moved) return false;
  raiseFloor(epoch);
  if (epoch - readFloor() >= MIN_ADVANCE_SECS) writeFloor(epoch);
  return true;
}

bool lowerProvenFloor(const int64_t epoch) {
  if (!plausible(epoch)) return false;
  bool lowered = false;
  portENTER_CRITICAL(&ramFloorLock);
  if (epoch < provenFloorValue) {
    provenFloorValue = epoch;
    ramFloor = epoch;
    lowered = true;
  }
  portEXIT_CRITICAL(&ramFloorLock);
  if (!lowered) return false;
  writeFloor(epoch);
  LOG_ERR("TIME", "The clock floor was ahead of the first sync: lowered to %lld", static_cast<long long>(epoch));
  return true;
}

void resetFloor() {
  portENTER_CRITICAL(&ramFloorLock);
  ramFloor = 0;
  provenFloorValue = 0;
  portEXIT_CRITICAL(&ramFloorLock);
  writeFloor(0);
  // A system clock that is too far ahead would be read again (and counted) before the next sync fixes it: put it where
  // no time is believed. Not MIN_VALID_EPOCH: that is the first second plausible() accepts, so the reader would read as
  // trusted at 2025-01-01 (and a second later still would). Zero is what a cold boot with no floor starts from.
  timeval tv = {0, 0};
  settimeofday(&tv, nullptr);
  LOG_ERR("TIME", "The clock check was reset: the floor is forgotten");
}

void setSyncPrepareHook(void (*hook)()) { prepareHook = hook; }

void startSync() {
  if (esp_sntp_enabled()) return;  // running; the sync callback handles the rest
  runPrepareHook();
  configureSntp();
}

uint32_t beginSync() {
  // configureSntp() (configTzTime) restarts SNTP if already running.
  const uint32_t before = syncCount.load(std::memory_order_acquire);
  runPrepareHook();
  configureSntp();
  return before;
}

bool syncCompleted(const uint32_t token) {
  // sntp_get_sync_status() resets COMPLETED back to RESET when it reports it
  // (see esp_sntp.h), so it must be read exactly once per pass. Reading it a
  // second time to decide the result turned every successful sync into a failure.
  const bool completed =
      sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED || syncCount.load(std::memory_order_acquire) != token;
  if (completed && !floorPolicy) note();
  return completed;
}

bool syncNow(const uint32_t timeoutMs) {
  const uint32_t token = beginSync();
  const unsigned long started = millis();
  const unsigned long deadline = started + timeoutMs;
  bool synced = false;
  while (static_cast<long>(deadline - millis()) > 0) {
    if (syncCompleted(token)) {
      synced = true;
      break;
    }
    delay(100);
  }
  if (!synced) LOG_ERR("TIME", "SNTP sync timed out after %lu ms", millis() - started);
  return synced;
}

int64_t trustedNow() {
  const int64_t now = static_cast<int64_t>(time(nullptr));
  // Never earlier than a time already seen; 0 while neither is trustworthy.
  const int64_t floor = raiseFloor(plausible(now) ? now : 0);
  return plausible(floor) ? floor : 0;
}

bool setServers(const char* const* servers, const size_t count) {
  if (servers == nullptr || count == 0) {
    configuredCount = 0;
    LOG_DBG("TIME", "SNTP servers: the defaults");
    return true;
  }
  if (count > MAX_SERVERS) return false;
  for (size_t i = 0; i < count; ++i)
    if (!acceptableServerName(servers[i])) return false;
  // The same list again changes nothing: a running client is left alone.
  if (count == configuredCount) {
    bool same = true;
    for (size_t i = 0; same && i < count; ++i) same = std::strcmp(configuredServers[i], servers[i]) == 0;
    if (same) return true;
  }
  // A running client may point at the texts about to change: stop it first.
  // The next sync starts it again with every slot set.
  if (esp_sntp_enabled()) esp_sntp_stop();
  for (size_t i = 0; i < count; ++i) {
    snprintf(configuredServers[i], sizeof(configuredServers[i]), "%s", servers[i]);
    configuredNames[i] = configuredServers[i];
  }
  configuredCount = count;
  LOG_DBG("TIME", "SNTP servers: %u configured", static_cast<unsigned>(count));
  return true;
}

void setUseDhcpServers(const bool enable) {
  if (enable == useDhcpServers) return;
  useDhcpServers = enable;
#if LWIP_DHCP_GET_NTP_SRV
  esp_sntp_servermode_dhcp(enable);
#endif
  LOG_DBG("TIME", "SNTP servers from DHCP: %s", enable ? "on" : "off");
}

}  // namespace trustedtime
