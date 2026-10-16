#pragma once

#include <esp_err.h>

#include <cstdint>

namespace message_tls {
// Each message poll needs a new sync. A restored NVS floor is insufficient. When the sync fails, the retained clock
// stands in if it is trusted (see useRetainedClock()). The installed network profile's SNTP servers (and its DHCP
// option) are applied first, here, so that every caller of a sync gets them.
bool prepareClock();

// The same sync as prepareClock(), in pieces, for a caller that must keep polling input while it runs (the Method
// shelf). beginClockSync() withdraws the clock permission, starts the sync and keeps Wi-Fi power-save off.
// pollClockSync() never waits: it reads the sync status once and answers. Synced grants the same permission
// prepareClock() would; Failed means the sync finished with an implausible time, or none is in flight (it was never
// begun, already ended, or was aborted). The deadline is the caller's; prepareClock() allows 20 s. A caller that
// gives up or is cancelled calls abortClockSync(), which is a no-op unless a sync is in flight, so a finished
// check can call it on every way out without touching a permission it earned.
enum class ClockSync : uint8_t { Pending, Synced, Failed };
void beginClockSync();
ClockSync pollClockSync();
void abortClockSync();

// After a sync failed or ran out of time: grants the HTTPS permission on the retained clock when ClockProvenance
// trusts it now (an unbroken count since a recorded sync, with an error bound of at most 10 minutes). Certificates
// are then checked over the widened window [now - bound, now + bound]. True at once when a sync permission is still
// current; false, with no permission, when the retained clock is not trusted.
bool useRetainedClock();

// Forgets every permission and that this boot ever synced: the SNTP permission, the retained one, and a sync in
// flight's completion. The person's "Reset the clock check" calls it, so that nothing that was granted on the clock
// before the reset (the 90 seconds a sync allows) is still honoured after it.
void withdrawClock();
// A sync set the clock this boot (a plausible time came from the network). It stays true until withdrawClock(): unlike
// the HTTPS permission it does not lapse after 90 s. Whether the clock is still good then is the retained clock's say.
bool clockSyncedThisBoot();

// What the current HTTPS permission rests on: Sntp (a sync in the last 90 s), Retained, or None (no permission).
enum class ClockSource : uint8_t { None, Sntp, Retained };
ClockSource clockSource();
bool clockReady();

// A time certificate dates can be checked against, and how far it can be off (seconds, 0 for a fresh sync).
struct CertificateClock {
  ClockSource source = ClockSource::None;
  int64_t now = 0;
  int64_t boundSeconds = 0;
};
// For checks outside HTTPS, such as an EAP-TLS join, which runs before there is a network to sync over: a sync from
// the last 90 s, else the retained clock if it is trusted now (which needs no network). False, with source None,
// when nothing vouches for the time: the dates cannot be checked then. Main task (it may read the RTC chip).
bool certificateClock(CertificateClock& out);
bool certificateClockTrusted();

// Wrap the SDK bundle verifier, then OR certificate validity flags.
esp_err_t attachDateCheckedBundle(void* sslConfig);
}  // namespace message_tls
