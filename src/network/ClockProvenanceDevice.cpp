#include <Arduino.h>
#include <HalClock.h>
#include <Logging.h>
#include <Preferences.h>
#include <TrustedTime.h>
#include <esp_attr.h>
#include <esp_random.h>
#include <esp_rtc_time.h>
#include <esp_system.h>
#include <sys/time.h>

#include "ClockProvenance.h"
#include "MessageTls.h"

// What each board really keeps between a sync and a later boot (freeink-sdk/docs/xteink-x4pro-support.md,
// xteink-x4c-support.md): the ESP32-S3's RTC timer runs from the internal 136 kHz RC oscillator
// (CONFIG_RTC_CLK_SRC_INT_RC; there is no 32 kHz crystal on the chip) and counts through deep sleep, as RTC memory
// survives it; a reset or power loss clears both. Both boards also carry a BM8563 RTC chip on I2C (0x51) with its own
// 32.768 kHz crystal, powered from the battery: it keeps counting through a reset, and its VL flag reports a stop
// (Rtc::now() refuses to read the time while it is set).
namespace clockprov {
namespace {
constexpr char PREFS_NAMESPACE[] = "cptime";  // TrustedTime's namespace; its floor is the key "floor"
constexpr char PREFS_KEY[] = "sync";

// The last sync, as RTC memory keeps it through deep sleep (reloaded from the image on any other boot).
RTC_DATA_ATTR SyncRecord rtcCopy;

// Main task only.
SyncRecord current;
bool timerContinuous = false;

bool readNvs(SyncRecord& out) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/true)) return false;
  const bool ok = prefs.isKey(PREFS_KEY) && prefs.getBytesLength(PREFS_KEY) == sizeof(SyncRecord) &&
                  prefs.getBytes(PREFS_KEY, &out, sizeof(SyncRecord)) == sizeof(SyncRecord);
  prefs.end();
  return ok && recordValid(out);
}

bool removeNvs() {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false)) return false;
  const bool ok = !prefs.isKey(PREFS_KEY) || prefs.remove(PREFS_KEY);
  prefs.end();
  return ok;
}

// What the floor may rise to on its own (trustedtime::note()): the RTC chip's time, less its error bound, and only for
// a chip whose time came from syncs it agreed with. Never the system clock.
int64_t vouchedNow() {
  if (!recordValid(current) || !current.confirmed || !current.chipSet) return 0;
  int64_t chipEpoch = 0;
  if (!halClock.readUtc(chipEpoch)) return 0;
  return vouchedTime(current, true, chipEpoch);
}
const trustedtime::FloorPolicy FLOOR_POLICY = {&vouchedNow};

bool writeNvs(const SyncRecord& record) {
  Preferences prefs;
  if (!prefs.begin(PREFS_NAMESPACE, /*readOnly=*/false)) return false;
  const bool ok = prefs.putBytes(PREFS_KEY, &record, sizeof(SyncRecord)) == sizeof(SyncRecord);
  prefs.end();
  return ok;
}
}  // namespace

void begin() {
  SyncRecord stored;
  const bool haveRecord = readNvs(stored);
  const bool deepSleepWake = esp_reset_reason() == ESP_RST_DEEPSLEEP;
  timerContinuous = haveRecord && timerContinuousOverBoot(deepSleepWake, rtcCopy, stored, esp_rtc_get_time_us());
  if (!timerContinuous) rtcCopy = SyncRecord{};
  current = haveRecord ? stored : SyncRecord{};
  // From here the persisted floor only rises by what a sync the chip agreed with, or the chip itself, vouches for.
  trustedtime::setFloorPolicy(&FLOOR_POLICY);
  LOG_INF("CLOCK", "Sync record: %s%s, timer %s, RTC chip %s", haveRecord ? "found" : "none",
          haveRecord && stored.confirmed ? " (confirmed)" : "", timerContinuous ? "continuous" : "restarted",
          haveRecord && stored.chipSet ? "set from it" : "not set from it");
}

Verdict recordSync() {
  timeval now = {};
  gettimeofday(&now, nullptr);
  // The clock has just been set by SNTP. Whether the sync is believed is the RTC chip's say (judgeSync): it counts on
  // its own crystal, from an earlier sync, and the network cannot change it. It is read before it is written.
  SyncInputs inputs;
  inputs.clockNow = static_cast<int64_t>(now.tv_sec);
  inputs.floorEpoch = trustedtime::provenFloor();
  inputs.previous = current;
  // The chip is read when it can witness (a record it was set from) and when there is no record at all (the first sync
  // may be behind a floor an earlier firmware left, and a chip that answers must agree with it).
  if (!recordValid(current) || current.chipSet) inputs.chipValid = halClock.readUtc(inputs.chipEpoch);
  const SyncJudgment judgment = judgeSync(inputs);
  if (judgment.verdict == Verdict::Refuse) {
    LOG_ERR("CLOCK", "Synced time %lld not recorded: %s (floor %lld)", static_cast<long long>(inputs.clockNow),
            reasonName(judgment.reason), static_cast<long long>(inputs.floorEpoch));
    return judgment.verdict;
  }
  if (judgment.verdict == Verdict::SessionOnly) {
    LOG_ERR("CLOCK", "Synced time %lld used for this session only: %s by %lld s",
            static_cast<long long>(inputs.clockNow), reasonName(judgment.reason),
            static_cast<long long>(judgment.chipOffsetS < 0 ? -judgment.chipOffsetS : judgment.chipOffsetS));
    return judgment.verdict;
  }
  SyncRecord record;
  record.magic = SyncRecord::MAGIC;
  record.id = esp_random();
  record.epoch = judgment.epoch;
  record.rtcMicros = esp_rtc_get_time_us();
  record.confirmed = judgment.confirmed ? 1 : 0;
  record.chipSet = halClock.writeUtc(record.epoch) ? 1 : 0;
  if (!recordValid(record)) return Verdict::Refuse;
  rtcCopy = record;
  current = record;
  timerContinuous = true;
  if (judgment.floorTo != 0) trustedtime::raiseProvenFloor(judgment.floorTo);
  if (judgment.lowerFloorTo != 0) trustedtime::lowerProvenFloor(judgment.lowerFloorTo);
  if (!writeNvs(record)) LOG_ERR("CLOCK", "Could not record the sync in NVS");
  LOG_INF("CLOCK", "Sync recorded: %s%s", reasonName(judgment.reason), record.chipSet ? "" : " (RTC chip not set)");
  return Verdict::Record;
}

void resetClockCheck() {
  trustedtime::resetFloor();
  rtcCopy = SyncRecord{};
  current = SyncRecord{};
  timerContinuous = false;
  // The 90 seconds a sync allows HTTPS (and the retained permission) were earned on a clock that is gone.
  message_tls::withdrawClock();
  if (!removeNvs()) LOG_ERR("CLOCK", "Could not clear the sync record from NVS");
}

Estimate retained(const int64_t windowMs) {
  Inputs inputs;
  inputs.record = current;
  inputs.timerContinuous = timerContinuous;
  inputs.rtcNowMicros = esp_rtc_get_time_us();
  inputs.windowMs = windowMs;
  inputs.floorEpoch = trustedtime::provenFloor();  // never behind what a sync or the chip proved (not the system clock)
  if (recordValid(current) && current.chipSet) inputs.chipValid = halClock.readUtc(inputs.chipEpoch);
  const Estimate result = estimate(inputs);
  LOG_INF("CLOCK", "Retained clock: %s (%s, age %lld s, bound %lld ms)", result.trusted ? "trusted" : "not trusted",
          result.oscillator == Oscillator::CrystalRtc ? "RTC chip" : "internal clock",
          static_cast<long long>(result.ageMs / 1000), static_cast<long long>(result.boundMs));
  return result;
}

}  // namespace clockprov
