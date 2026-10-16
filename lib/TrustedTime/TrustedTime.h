#pragma once

#include <cstddef>
#include <cstdint>

// A monotonic floor under the system clock, for loan-expiry enforcement.
//
// The ESP32-C3 keeps time through deep sleep but loses it on power-off, and
// the device has no battery-backed RTC — so a cold boot starts near epoch 0
// and a date-based check would never fire offline. This module persists the
// last known-good time in NVS (on-flash, not on the removable SD card) and
// restores it into the system clock at boot, so time only ever moves forward
// across power cycles. Real time snaps in whenever Wi-Fi is up via SNTP.
//
// The floor can lag real time while the device sits powered off; it can never
// run behind a moment the device has already seen. Enforcement that needs a
// trustworthy "now" uses trustedNow() and fails closed when it returns 0.
namespace trustedtime {

// Restore the persisted floor into the system clock. Call once at boot,
// before anything reads time().
void init();

// A time that is not between 2025-01-01 and 2100-01-01 is never a time the reader has seen: it is not counted by
// trustedNow() and never reaches the floor (an SNTP reply from the far future would otherwise be kept in NVS for good).
constexpr int64_t MIN_VALID_EPOCH = 1735689600LL;  // 2025-01-01 UTC
constexpr int64_t MAX_VALID_EPOCH = 4102444800LL;  // 2100-01-01 UTC, the bound message_tls uses for certificate dates

// Persist the floor when the clock advanced past it. Cheap (one NVS read,
// a write only when it moved). Call at sleep entry and after a time sync.
// With a floor policy (below) it persists what the policy can vouch for, never the raw clock.
void note();

// ---- the floor policy ----------------------------------------------------------------------------------------------
//
// The persisted floor only rises, so a time that is too high stays: one forward-spoofed SNTP reply written to NVS keeps
// the reader refusing syncs for as long as the lie is long. A policy keeps the raw system clock away from NVS. With one
// installed (clockprov::begin() does it, once, after init()):
//   - note() persists vouchedNow(), a time something other than the system clock agrees with (the RTC chip, set from an
//     earlier sync and read back), or nothing; and the sync callbacks persist nothing;
//   - the "proven floor" is the persisted floor plus what accepted syncs and vouched times raised it to, and never the
//     raw clock. The retained clock is held to it (a clock that ran fast through a sleep must not veto the exact chip);
//   - the rest of the monotonic floor (trustedNow(), the RAM high-water mark of the clock) is unchanged, so TLS dates
//     cannot be taken back by a backward step within a boot.
// Without a policy everything is as it was.
struct FloorPolicy {
  // A time the reader can vouch for now, or 0. Main task only (it may read the RTC chip).
  int64_t (*vouchedNow)();
};
void setFloorPolicy(const FloorPolicy* policy);
// The persisted floor, raised by what an accepted sync or a vouched time raised it to. Never the raw clock.
int64_t provenFloor();
// An accepted sync, or a vouched time, raises the proven floor (and the monotonic floor) and persists it. False when it
// does not move it or is not a plausible time.
bool raiseProvenFloor(int64_t epoch);
// The one way down, for a reader whose floor was left ahead of true time by an earlier firmware (which persisted the
// raw internal clock at every sleep): lowers the proven floor, the monotonic floor and the persisted floor to `epoch`.
// Only ever called for the first sync a reader makes after it lost its clock record, and only when the clock record
// judges that sync (clockprov::judgeSync); everything after it is held to the new floor as before. False, with nothing
// changed, when `epoch` is not below the floor or not a plausible time.
bool lowerProvenFloor(int64_t epoch);
// The person's recovery ("Reset the clock check"): forgets the persisted and the RAM floors, and sets the system clock
// to zero so that the reader reads as having no time at all (trustedNow() is 0) until the next sync, whatever the clock
// said before. It is not set to the earliest plausible time: that is already one trustedNow() believes. The next sync
// starts afresh.
void resetFloor();

// Run before a sync starts: startSync() when it will start SNTP, beginSync() and so syncNow(). The firmware installs
// the installed network profile's time-server settings here, so that every caller of a sync gets them and none has to
// remember.
void setSyncPrepareHook(void (*hook)());

// Kick off a non-blocking SNTP sync; the sync callback persists the floor.
// Call whenever a Wi-Fi station connection comes up. No-op while running.
void startSync();

// Blocking SNTP sync with a deadline, for callers that need the wall clock
// right now (e.g. progress-sync timestamps). Returns true when synced.
bool syncNow(uint32_t timeoutMs);

// The two halves of syncNow(), for a caller that must keep polling something
// else (input) while the sync runs. beginSync() (re)starts SNTP exactly as
// syncNow() does and returns a token; call syncCompleted(token) from the
// caller's own loop. The deadline is the caller's. syncCompleted() reads the
// SNTP status once per call (the read consumes it) and, like syncNow(), also
// treats a sync callback since beginSync() as completion. When it reports
// true it has persisted the floor, as syncNow() does.
uint32_t beginSync();
bool syncCompleted(uint32_t token);

// Epoch seconds when the clock is trustworthy (a plausible present-day
// value, restored or synced), else 0. Never earlier than a time already seen
// this boot or restored from the floor, so a backward clock step cannot undo
// an expiry. Callers enforcing a date fail closed on 0.
int64_t trustedNow();

// The SNTP servers every sync from now on asks, in order: at most 3 names (host
// names or IPv4 literals), copied into static storage. nullptr or a count of 0
// restores the defaults (pool.ntp.org, time.google.com, time.cloudflare.com).
// False, with nothing changed, for more than 3 names or a name that is empty,
// longer than 253 characters, or has spaces or control characters. A sync
// already running keeps its servers (the same list again changes nothing, not even that); the next one uses these.
bool setServers(const char* const* servers, size_t count);

// Also ask the NTP server a DHCP lease offers (lwIP's DHCP option 42), ahead of
// the configured servers. lwIP takes the server from the lease as it arrives,
// so turn this on before joining. Off by default.
void setUseDhcpServers(bool enable);

}  // namespace trustedtime
