#include "MessageTls.h"

#include <Arduino.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <esp_crt_bundle.h>
#include <esp_sntp.h>
#include <mbedtls/build_info.h>
#include <mbedtls/ssl.h>
#include <sys/time.h>

#include <atomic>
#include <optional>

#include "CertificateDates.h"
#include "ClockProvenance.h"
#include "NetworkProfileDevice.h"
#include "WifiPowerSaveGuard.h"

// This adapter is reviewed against the installed prebuilt SDK. A changed
// config layout or bundle verifier requires explicit review before rebasing.
#if MBEDTLS_VERSION_NUMBER != 0x03060600
#error "Review Messages certificate-date adapter for this Mbed TLS version"
#endif

namespace message_tls {
namespace {
// lwIP's SNTP client waits a random 0-5 s (CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY)
// before its first request, so a 5 s budget failed on hardware every time.
constexpr uint32_t SYNC_TIMEOUT_MS = 20000;
constexpr uint32_t CLOCK_WINDOW_MS = 90000;
using Verify = int (*)(void*, mbedtls_x509_crt*, int, uint32_t*);
std::atomic<Verify> bundleVerifier{nullptr};
std::atomic<uint32_t> syncMillis{0};
std::atomic<bool> synchronized{false};
// Some sync this boot set a plausible time (kept until the person resets the clock check).
std::atomic<bool> syncedThisBoot{false};

// The permission a failed sync may fall back on (useRetainedClock()): the retained clock's estimate when it was
// granted, and its error bound at the end of the window. Written on the main task before the flag is set.
std::atomic<bool> retainedGranted{false};
std::atomic<uint32_t> retainedMillis{0};
int64_t retainedEpoch = 0;
int64_t retainedBound = 0;

// A sync started by beginClockSync() and not yet ended: the power-save guard it holds (engaged while it runs), the
// token that tells pollClockSync() which sync to look for, and when it began.
std::optional<WifiPowerSaveGuard> syncGuard;
uint32_t syncToken = 0;
unsigned long syncStarted = 0;

// Every sync starts by withdrawing the permission and discarding any completion left from an earlier one.
void discardClock() {
  synchronized.store(false, std::memory_order_release);
  retainedGranted.store(false, std::memory_order_release);
  // The documented status setter is thread safe. Discard any prior completion
  // before reusing CrossPoint's bounded sync; never accept a restored floor.
  esp_sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
}

// Every sync ends here, whether prepareClock() waited for it or pollClockSync() saw it complete.
bool concludeClock(const bool synced, const unsigned long started) {
  // The system clock the sync just set must itself be a time: an answer from beyond 2100 is not a sync, whatever the
  // monotonic floor (which never counts it) still says.
  timeval raw = {};
  gettimeofday(&raw, nullptr);
  if (!synced || !plausibleEpoch(static_cast<int64_t>(raw.tv_sec)) || !plausibleEpoch(trustedtime::trustedNow())) {
    LOG_ERR("HTTP", "Messages clock sync failed; HTTPS skipped");
    return false;
  }
  LOG_INF("HTTP", "Messages clock synced in %lu ms", millis() - started);
  syncMillis.store(millis(), std::memory_order_relaxed);
  synchronized.store(true, std::memory_order_release);
  syncedThisBoot.store(true, std::memory_order_release);
  // What the sync is believed for beyond this session is the clock record's decision (it logs a refusal or a chip that
  // disagrees); the session itself uses the sync as it always did.
  clockprov::recordSync();
  return true;
}

bool sntpReady() {
  return synchronized.load(std::memory_order_acquire) &&
         static_cast<uint32_t>(millis() - syncMillis.load(std::memory_order_relaxed)) < CLOCK_WINDOW_MS &&
         plausibleEpoch(trustedtime::trustedNow());
}

bool retainedReady() {
  return retainedGranted.load(std::memory_order_acquire) &&
         static_cast<uint32_t>(millis() - retainedMillis.load(std::memory_order_relaxed)) < CLOCK_WINDOW_MS;
}

// The retained estimate carried forward by the time since it was granted.
int64_t retainedNow() {
  return retainedEpoch + static_cast<uint32_t>(millis() - retainedMillis.load(std::memory_order_relaxed)) / 1000;
}

int reject(void*, mbedtls_x509_crt*, int, uint32_t* flags) {
  if (flags) *flags |= MBEDTLS_X509_BADCERT_OTHER;
  return MBEDTLS_ERR_X509_FATAL_ERROR;
}

UtcDate date(const mbedtls_x509_time& value) {
  return {value.year, value.mon, value.day, value.hour, value.min, value.sec};
}
int verify(void*, mbedtls_x509_crt* certificate, int depth, uint32_t* flags) {
  const auto delegate = bundleVerifier.load(std::memory_order_acquire);
  if (!delegate || !certificate || !flags) return MBEDTLS_ERR_X509_FATAL_ERROR;
  // The bundle may clear its NOT_TRUSTED flag after signature verification.
  // Date flags are added afterwards, and no other verifier flags are cleared.
  const int result = delegate(nullptr, certificate, depth, flags);
  if (result != 0) return result;
  const ClockSource source = clockSource();
  if (source == ClockSource::None) {
    *flags |= MBEDTLS_X509_BADCERT_OTHER;
    return 0;
  }
  // The SDK's synthetic trust-anchor placeholder contains no validity dates.
  // Its exact internal pointer is recognized by the SDK, not by peer input.
  if (esp_crt_bundle_in_use(certificate)) return 0;
  const UtcDate from = date(certificate->valid_from);
  const UtcDate to = date(certificate->valid_to);
  // A retained clock is known only to within its bound: the certificate must be valid over the whole window.
  const DateResult dates = source == ClockSource::Sntp ? checkDates(from, to, trustedtime::trustedNow())
                                                       : checkDatesWithin(from, to, retainedNow(), retainedBound);
  switch (dates) {
    case DateResult::Expired:
      *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
      break;
    case DateResult::Future:
      *flags |= MBEDTLS_X509_BADCERT_FUTURE;
      break;
    case DateResult::InvalidClock:
    case DateResult::Malformed:
      *flags |= MBEDTLS_X509_BADCERT_OTHER;
      break;
    case DateResult::Valid:
      break;
  }
  return 0;
}
}  // namespace

bool prepareClock() {
  // The installed network profile's time servers (and its DHCP option) belong to every sync, so they are applied here,
  // not by each caller: the Notes poll, the Method shelf, Check connection and whatever asks for a clock next.
  netprofile::applyTimeSettings();
  discardClock();
  // Modem sleep can drop or delay the single UDP reply; keep the radio awake.
  WifiPowerSaveGuard psGuard;
  const unsigned long started = millis();
  if (concludeClock(trustedtime::syncNow(SYNC_TIMEOUT_MS), started)) return true;
  return useRetainedClock();
}

void beginClockSync() {
  netprofile::applyTimeSettings();  // as in prepareClock()
  discardClock();
  // Held until the sync ends or is aborted. emplace() replaces a guard left by an earlier, unfinished sync.
  syncGuard.emplace();
  syncStarted = millis();
  syncToken = trustedtime::beginSync();
}

ClockSync pollClockSync() {
  if (!syncGuard) return ClockSync::Failed;
  if (!trustedtime::syncCompleted(syncToken)) return ClockSync::Pending;
  const bool granted = concludeClock(true, syncStarted);
  syncGuard.reset();
  return granted ? ClockSync::Synced : ClockSync::Failed;
}

// beginClockSync() already withdrew the permission and nothing has granted it since, so only the power-save guard is
// left to drop. Resetting an empty optional does nothing.
void abortClockSync() { syncGuard.reset(); }

bool useRetainedClock() {
  if (sntpReady()) return true;
  const clockprov::Estimate retained = clockprov::retained(CLOCK_WINDOW_MS);
  if (!retained.trusted) {
    LOG_ERR("HTTP", "No trusted clock; HTTPS skipped");
    return false;
  }
  retainedEpoch = retained.nowEpoch;
  retainedBound = clockprov::boundSeconds(retained.boundMs);
  retainedMillis.store(millis(), std::memory_order_relaxed);
  retainedGranted.store(true, std::memory_order_release);
  LOG_INF("HTTP", "HTTPS on the retained clock (within %lld s)", static_cast<long long>(retainedBound));
  return true;
}

ClockSource clockSource() {
  if (sntpReady()) return ClockSource::Sntp;
  if (retainedReady()) return ClockSource::Retained;
  return ClockSource::None;
}

bool clockReady() { return clockSource() != ClockSource::None; }

void withdrawClock() {
  discardClock();
  syncedThisBoot.store(false, std::memory_order_release);
  syncGuard.reset();
}

bool clockSyncedThisBoot() { return syncedThisBoot.load(std::memory_order_acquire); }

bool certificateClock(CertificateClock& out) {
  out = {};
  if (sntpReady()) {
    out = {ClockSource::Sntp, trustedtime::trustedNow(), 0};
    return true;
  }
  if (retainedReady()) {
    out = {ClockSource::Retained, retainedNow(), retainedBound};
    return true;
  }
  const clockprov::Estimate retained = clockprov::retained(CLOCK_WINDOW_MS);
  if (!retained.trusted) return false;
  out = {ClockSource::Retained, retained.nowEpoch, clockprov::boundSeconds(retained.boundMs)};
  return true;
}

bool certificateClockTrusted() {
  CertificateClock clock;
  return certificateClock(clock);
}

esp_err_t attachDateCheckedBundle(void* sslConfig) {
  if (!sslConfig) return ESP_FAIL;
  auto* config = static_cast<mbedtls_ssl_config*>(sslConfig);
  // IDF 5.5.5 ignores this hook's return value. Install a denying callback
  // before setup, and again on any failure after the bundle overwrites it.
  mbedtls_ssl_conf_authmode(config, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_verify(config, reject, nullptr);
  if (!clockReady()) return ESP_FAIL;
  const esp_err_t result = esp_crt_bundle_attach(sslConfig);
  if (result != ESP_OK) {
    mbedtls_ssl_conf_verify(config, reject, nullptr);
    return result;
  }
  const auto original = config->MBEDTLS_PRIVATE(f_vrfy);
  // IDF 5.5.5 installs a single stateless callback with a null context. Fail
  // closed if that contract changes; never guess an opaque context's lifetime.
  if (!original || original == verify || config->MBEDTLS_PRIVATE(p_vrfy) != nullptr) {
    mbedtls_ssl_conf_verify(config, reject, nullptr);
    return ESP_FAIL;
  }
  Verify expected = nullptr;
  if (!bundleVerifier.compare_exchange_strong(expected, original, std::memory_order_acq_rel) && expected != original) {
    mbedtls_ssl_conf_verify(config, reject, nullptr);
    return ESP_FAIL;
  }
  mbedtls_ssl_conf_verify(config, verify, nullptr);
  return ESP_OK;
}
}  // namespace message_tls
