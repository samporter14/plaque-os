#include <TrustedTime.h>
#include <esp_crt_bundle.h>
#include <esp_sntp.h>
#include <esp_wifi.h>
#include <mbedtls/build_info.h>
#include <mbedtls/x509_crt.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "network/CertificateDates.h"
#include "network/ClockProvenance.h"
#include "network/MessageTls.h"
#include "network/NetworkProfileDevice.h"

#ifdef MBEDTLS_HAVE_TIME_DATE
#error "This regression harness must reproduce the installed date-disabled library"
#endif
namespace {
constexpr int64_t NOW = 1791028800;
uint32_t uptime = 1000;
int64_t clockEpoch = NOW;
bool ntpSuccess = true, statusReset = false, opaqueContext = false, clearTrust = false;
bool bundleFailure = false, missingVerifier = false, changedVerifier = false;
const mbedtls_x509_crt syntheticAnchor{};
int syncCalls = 0, checks = 0;
// Wi-Fi power-save as the radio would hold it, so the tests can see when WifiPowerSaveGuard is engaged.
wifi_ps_type_t powerSave = WIFI_PS_MIN_MODEM;
// The non-blocking halves of the sync, as scripted by the test: syncCompleted() answers false until it has been
// polled `pollsUntilDone` times (never, when it is negative).
int beginCalls = 0, polls = 0, pollsUntilDone = 0;
uint32_t lastToken = 0;
// The retained clock as ClockProvenance would report it (its own rules are covered by test/clock_provenance).
clockprov::Estimate scriptedRetained;
int recordedSyncs = 0, retainedQueries = 0;
int64_t lastRetainedWindow = 0;
std::string fixtureDirectory;
// What happened, in order, for the tests that care about it: the profile's time settings (S3) against the sync.
std::vector<std::string> callOrder;
void expect(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << '\n';
    std::exit(1);
  }
  ++checks;
  std::cout << "PASS: " << label << '\n';
}
void parse(mbedtls_x509_crt& cert, const std::string& filename) {
  mbedtls_x509_crt_init(&cert);
  std::ifstream input(fixtureDirectory + "/" + filename, std::ios::binary);
  std::string body{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  expect(!body.empty(), "fixture readable");
  const bool pem = filename.ends_with(".pem");
  expect(mbedtls_x509_crt_parse(&cert, reinterpret_cast<const unsigned char*>(body.c_str()),
                                body.size() + (pem ? 1 : 0)) == 0,
         "real certificate parser accepts fixture");
}
int bundle(void*, mbedtls_x509_crt*, int, uint32_t* flags) {
  // Native trust comes from the real synthetic root. An extra direct callback
  // check models the SDK bundle clearing its sole NOT_TRUSTED flag.
  if (clearTrust && *flags == MBEDTLS_X509_BADCERT_NOT_TRUSTED) *flags = 0;
  return 0;
}
int alternateBundle(void*, mbedtls_x509_crt*, int, uint32_t*) { return 0; }
uint32_t verifyCertificate(const std::string& filename, bool wrapped, const std::string& rootName = "root.pem") {
  mbedtls_x509_crt peer, root;
  parse(peer, filename);
  parse(root, rootName);
  mbedtls_ssl_config config;
  mbedtls_ssl_config_init(&config);
  expect(message_tls::prepareClock(), "fresh sync before verification");
  expect(message_tls::attachDateCheckedBundle(&config) == ESP_OK, "bundle adapter attaches");
  uint32_t flags = 0;
  const int result = mbedtls_x509_crt_verify(&peer, &root, nullptr, "notes.test", &flags,
                                             wrapped ? config.MBEDTLS_PRIVATE(f_vrfy) : nullptr,
                                             wrapped ? config.MBEDTLS_PRIVATE(p_vrfy) : nullptr);
  expect((result == 0) == (flags == 0), "real chain verifier agrees with its flags");
  mbedtls_ssl_config_free(&config);
  mbedtls_x509_crt_free(&peer);
  mbedtls_x509_crt_free(&root);
  return flags;
}
}  // namespace
int errorLogs = 0;
unsigned long millis() { return uptime; }
void esp_sntp_set_sync_status(sntp_sync_status_t value) { statusReset = value == SNTP_SYNC_STATUS_RESET; }
esp_err_t esp_wifi_get_ps(wifi_ps_type_t* type) {
  *type = powerSave;
  return ESP_OK;
}
esp_err_t esp_wifi_set_ps(wifi_ps_type_t type) {
  powerSave = type;
  return ESP_OK;
}
// The installed profile's time servers and DHCP option are the profile's business (test/network_profile covers what
// they are); message_tls must apply them before a sync, whoever asked for it.
namespace netprofile {
void applyTimeSettings() { callOrder.push_back("apply"); }
}  // namespace netprofile
namespace trustedtime {
bool syncNow(uint32_t timeout) {
  callOrder.push_back("sync");
  ++syncCalls;
  expect(timeout == 20000, "sync deadline remains bounded");
  expect(statusReset, "old sync completion discarded");
  expect(powerSave == WIFI_PS_NONE, "radio kept awake for the whole sync");
  statusReset = false;
  return ntpSuccess;
}
uint32_t beginSync() {
  callOrder.push_back("begin");
  ++beginCalls;
  expect(statusReset, "old sync completion discarded before the sync begins");
  expect(powerSave == WIFI_PS_NONE, "radio kept awake before the sync begins");
  statusReset = false;
  return 1000 + beginCalls;
}
bool syncCompleted(uint32_t token) {
  lastToken = token;
  ++polls;
  return pollsUntilDone >= 0 && polls >= pollsUntilDone;
}
int64_t trustedNow() { return clockEpoch; }
}  // namespace trustedtime
esp_err_t esp_crt_bundle_attach(void* pointer) {
  auto* config = static_cast<mbedtls_ssl_config*>(pointer);
  mbedtls_ssl_conf_verify(config,
                          missingVerifier   ? nullptr
                          : changedVerifier ? alternateBundle
                                            : bundle,
                          opaqueContext ? config : nullptr);
  return bundleFailure ? ESP_FAIL : ESP_OK;
}
bool esp_crt_bundle_in_use(const mbedtls_x509_crt* certificate) { return certificate == &syntheticAnchor; }
namespace clockprov {
void begin() {}
Verdict recordSync() {
  ++recordedSyncs;
  return Verdict::Record;
}
Estimate retained(int64_t windowMs) {
  ++retainedQueries;
  lastRetainedWindow = windowMs;
  return scriptedRetained;
}
}  // namespace clockprov

namespace {
clockprov::Estimate trustedRetained(int64_t now, int64_t boundMs) {
  clockprov::Estimate estimate;
  estimate.trusted = true;
  estimate.oscillator = clockprov::Oscillator::CrystalRtc;
  estimate.nowEpoch = now;
  estimate.ageMs = 3600000;
  estimate.boundMs = boundMs;
  return estimate;
}

// Grants the retained permission through the blocking path, as Messages would after its sync failed.
void grantRetained(int64_t now, int64_t boundMs) {
  ntpSuccess = false;
  scriptedRetained = trustedRetained(now, boundMs);
  expect(message_tls::prepareClock(), "a failed sync falls back on a trusted retained clock");
}

uint32_t verifyOnCurrentClock(const std::string& filename) {
  mbedtls_x509_crt peer, root;
  parse(peer, filename);
  parse(root, "root.pem");
  mbedtls_ssl_config config;
  mbedtls_ssl_config_init(&config);
  expect(message_tls::attachDateCheckedBundle(&config) == ESP_OK, "bundle adapter attaches on the current clock");
  uint32_t flags = 0;
  const int result = mbedtls_x509_crt_verify(&peer, &root, nullptr, "notes.test", &flags,
                                             config.MBEDTLS_PRIVATE(f_vrfy), config.MBEDTLS_PRIVATE(p_vrfy));
  expect((result == 0) == (flags == 0), "real chain verifier agrees with its flags");
  mbedtls_ssl_config_free(&config);
  mbedtls_x509_crt_free(&peer);
  mbedtls_x509_crt_free(&root);
  return flags;
}
}  // namespace

// SNTP first; when it fails, the retained clock with the widened date check; otherwise nothing.
void testRetainedClock() {
  using message_tls::ClockSource;
  constexpr int64_t VALID_FROM = 1790812800;  // valid.pem: 2026-10-01T00:00:00Z
  constexpr int64_t VALID_TO = 1798761600;    // valid.pem: 2027-01-01T00:00:00Z
  uptime = 200000;
  clockEpoch = NOW;

  // A successful sync is recorded for the next boot, and the permission rests on it.
  ntpSuccess = true;
  const int recordedBefore = recordedSyncs;
  const int queriesBefore = retainedQueries;
  expect(message_tls::prepareClock(), "fresh sync");
  expect(recordedSyncs == recordedBefore + 1, "a successful sync is recorded (NVS, RTC memory, RTC chip)");
  expect(retainedQueries == queriesBefore, "a successful sync never asks the retained clock");
  expect(message_tls::clockSource() == ClockSource::Sntp, "the permission rests on SNTP");
  expect(message_tls::useRetainedClock() && message_tls::clockSource() == ClockSource::Sntp,
         "with SNTP current, the retained clock is not needed and changes nothing");
  message_tls::CertificateClock clock;
  expect(message_tls::certificateClock(clock) && clock.source == ClockSource::Sntp && clock.now == NOW &&
             clock.boundSeconds == 0,
         "a fresh sync vouches for certificate dates exactly");

  // The sync fails and the retained clock is not trusted: no HTTPS at all.
  ntpSuccess = false;
  scriptedRetained = clockprov::Estimate{};
  const int recordedAfterSync = recordedSyncs;
  expect(!message_tls::prepareClock(), "failed sync without a trusted retained clock: offline");
  expect(recordedSyncs == recordedAfterSync, "a failed sync records nothing");
  expect(lastRetainedWindow == 90000, "the retained clock is judged at the end of the 90 s window");
  expect(message_tls::clockSource() == ClockSource::None && !message_tls::clockReady(), "no permission");
  expect(!message_tls::certificateClockTrusted(), "nothing vouches for certificate dates");
  mbedtls_ssl_config config;
  mbedtls_ssl_config_init(&config);
  expect(message_tls::attachDateCheckedBundle(&config) == ESP_FAIL, "no clock, no TLS");
  mbedtls_ssl_config_free(&config);

  // The sync fails and the retained clock is trusted: HTTPS on it, with the widened check.
  grantRetained(NOW, 300000);
  expect(message_tls::clockSource() == ClockSource::Retained && message_tls::clockReady(), "retained permission");
  expect(message_tls::certificateClock(clock) && clock.source == ClockSource::Retained && clock.now == NOW &&
             clock.boundSeconds == 300,
         "certificate clock reports the retained time and its bound");
  expect(verifyOnCurrentClock("valid.pem") == 0, "retained: a certificate valid around now is accepted");
  expect(verifyOnCurrentClock("boundary.pem") & MBEDTLS_X509_BADCERT_FUTURE,
         "retained: a certificate valid only at this instant is refused (the window starts before it)");
  expect(verifyOnCurrentClock("expired.pem") & MBEDTLS_X509_BADCERT_EXPIRED, "retained: expired still rejected");
  expect(verifyOnCurrentClock("future.pem") & MBEDTLS_X509_BADCERT_FUTURE, "retained: not yet valid still rejected");
  expect(verifyOnCurrentClock("wrong-host.pem") & MBEDTLS_X509_BADCERT_CN_MISMATCH,
         "retained: host name still checked");
  expect(verifyOnCurrentClock("untrusted.pem") & MBEDTLS_X509_BADCERT_NOT_TRUSTED, "retained: chain still checked");

  // The widened window at the edges of the validity period.
  grantRetained(VALID_FROM + 300, 300000);
  expect(verifyOnCurrentClock("valid.pem") == 0, "window starting exactly at notBefore: valid");
  grantRetained(VALID_FROM + 299, 300000);
  expect(verifyOnCurrentClock("valid.pem") & MBEDTLS_X509_BADCERT_FUTURE, "window starting a second earlier: future");
  grantRetained(VALID_TO - 300, 300000);
  expect(verifyOnCurrentClock("valid.pem") == 0, "window ending exactly at notAfter: valid");
  grantRetained(VALID_TO - 299, 300000);
  expect(verifyOnCurrentClock("valid.pem") & MBEDTLS_X509_BADCERT_EXPIRED, "window ending a second later: expired");
  grantRetained(VALID_TO - 300, 299001);  // 299.001 s rounds up to a 300 s window
  expect(verifyOnCurrentClock("valid.pem") == 0, "a bound in milliseconds is rounded up to whole seconds");

  // The retained time moves on with the uptime counter while the permission lasts, then the permission ends.
  grantRetained(VALID_TO - 310, 300000);
  uptime += 10000;
  expect(verifyOnCurrentClock("valid.pem") == 0, "ten seconds later the window ends exactly at notAfter");
  uptime += 1000;
  expect(verifyOnCurrentClock("valid.pem") & MBEDTLS_X509_BADCERT_EXPIRED, "and one second after that it does not");
  uptime += 79000 - 1;
  expect(message_tls::clockSource() == ClockSource::Retained, "the retained permission lasts the 90 s window");
  uptime += 1;
  expect(message_tls::clockSource() == ClockSource::None, "and no longer");

  // A new sync takes the retained permission away; a poll-driven sync that fails can fall back explicitly.
  grantRetained(NOW, 300000);
  pollsUntilDone = -1;
  polls = 0;
  message_tls::beginClockSync();
  expect(message_tls::clockSource() == ClockSource::None, "a new sync withdraws the retained permission");
  message_tls::abortClockSync();
  expect(message_tls::useRetainedClock() && message_tls::clockSource() == ClockSource::Retained,
         "the caller's sync deadline passed: it falls back on the retained clock");
  scriptedRetained = clockprov::Estimate{};
  message_tls::beginClockSync();
  message_tls::abortClockSync();
  expect(!message_tls::useRetainedClock() && !message_tls::clockReady(), "and not when it is untrusted");

  // An EAP-TLS join before any network: the retained clock vouches without granting HTTPS.
  scriptedRetained = trustedRetained(NOW, 5000);
  expect(message_tls::certificateClock(clock) && clock.source == ClockSource::Retained && clock.boundSeconds == 5,
         "certificate clock from the retained clock alone");
  expect(!message_tls::clockReady(), "asking does not grant the HTTPS permission");
  scriptedRetained = clockprov::Estimate{};
  expect(!message_tls::certificateClock(clock) && clock.source == ClockSource::None,
         "with nothing trusted the dates cannot be checked");

  // The widened rule itself, at its edges.
  const message_tls::UtcDate from{2026, 10, 1, 0, 0, 0}, to{2027, 1, 1, 0, 0, 0};
  using message_tls::DateResult;
  expect(message_tls::checkDatesWithin(from, to, NOW, 0) == message_tls::checkDates(from, to, NOW),
         "a bound of zero is the plain check");
  expect(message_tls::checkDatesWithin(from, to, VALID_FROM + 600, 600) == DateResult::Valid, "notBefore edge");
  expect(message_tls::checkDatesWithin(from, to, VALID_FROM + 599, 600) == DateResult::Future, "notBefore edge - 1");
  expect(message_tls::checkDatesWithin(from, to, VALID_TO - 600, 600) == DateResult::Valid, "notAfter edge");
  expect(message_tls::checkDatesWithin(from, to, VALID_TO - 599, 600) == DateResult::Expired, "notAfter edge + 1");
  expect(message_tls::checkDatesWithin(from, to, NOW, -1) == DateResult::InvalidClock, "negative bound refused");
  expect(message_tls::checkDatesWithin(from, to, 0, 10) == DateResult::InvalidClock, "missing clock refused");
  expect(message_tls::checkDatesWithin(to, from, NOW, 10) == DateResult::Malformed, "reversed interval refused");
  ntpSuccess = true;
}

// beginClockSync() / pollClockSync() / abortClockSync(): the same permission prepareClock() grants, in pieces.
// S3: whoever syncs gets the installed network profile's time servers, applied before the sync starts.
void testTimeSettingsReachEverySync() {
  uptime = 400000;
  clockEpoch = NOW;
  ntpSuccess = true;
  callOrder.clear();
  expect(message_tls::prepareClock(), "a blocking sync");
  expect((callOrder == std::vector<std::string>{"apply", "sync"}),
         "prepareClock() applies the time settings, then syncs");
  callOrder.clear();
  pollsUntilDone = 1;
  polls = 0;
  message_tls::beginClockSync();
  expect((callOrder == std::vector<std::string>{"apply", "begin"}),
         "beginClockSync() applies the time settings, then begins the sync");
  message_tls::abortClockSync();
  callOrder.clear();
  ntpSuccess = false;
  scriptedRetained = clockprov::Estimate{};
  expect(!message_tls::prepareClock(), "a sync that fails");
  expect((callOrder == std::vector<std::string>{"apply", "sync"}), "and the settings were applied for it too");
  ntpSuccess = true;
}

void testClockSyncInPieces() {
  using message_tls::ClockSync;
  const auto begin = [](int pollsToFinish) {
    polls = 0;
    pollsUntilDone = pollsToFinish;
    message_tls::beginClockSync();
  };
  powerSave = WIFI_PS_MIN_MODEM;
  clockEpoch = NOW;
  uptime = 50000;

  // Nothing begun: nothing to poll, and an abort touches nothing.
  expect(message_tls::pollClockSync() == ClockSync::Failed, "no sync in flight: failed, not pending forever");
  message_tls::abortClockSync();
  expect(powerSave == WIFI_PS_MIN_MODEM, "abort with nothing in flight leaves the radio alone");

  // A sync that takes a few polls: pending without permission, radio awake, then synced.
  begin(3);
  expect(beginCalls > 0 && lastToken == 0, "begin does not poll");
  expect(!message_tls::clockReady(), "begin withdraws the clock permission");
  expect(statusReset == false, "the old completion was discarded and the sync's own begin consumed that note");
  expect(powerSave == WIFI_PS_NONE, "power-save stays off while the sync runs");
  expect(message_tls::pollClockSync() == ClockSync::Pending, "first poll: pending");
  expect(lastToken == 1000u + static_cast<uint32_t>(beginCalls), "poll asks about the sync begin() started");
  expect(message_tls::pollClockSync() == ClockSync::Pending, "second poll: pending");
  expect(!message_tls::clockReady() && powerSave == WIFI_PS_NONE, "still no permission, radio still awake");
  expect(message_tls::pollClockSync() == ClockSync::Synced, "third poll: synced");
  expect(message_tls::clockReady(), "synced grants the same permission prepareClock() does");
  expect(powerSave == WIFI_PS_MIN_MODEM, "power-save restored when the sync ends");
  expect(message_tls::pollClockSync() == ClockSync::Failed, "a sync that has ended is no longer in flight");
  expect(message_tls::clockReady(), "polling after the end does not take the permission back");
  uptime += 89999;
  expect(message_tls::clockReady(), "permission lasts the 90 s window");
  uptime += 1;
  expect(!message_tls::clockReady(), "and no longer");

  // abortSync() after a successful sync is harmless: a finished check calls it on every way out.
  uptime = 70000;
  begin(1);
  expect(message_tls::pollClockSync() == ClockSync::Synced, "synced on the first poll");
  message_tls::abortClockSync();
  expect(message_tls::clockReady(), "abort after Synced keeps the permission the sync earned");
  expect(powerSave == WIFI_PS_MIN_MODEM, "and does not touch power-save twice");

  // Abort while the sync runs: radio restored, permission still withdrawn, nothing in flight.
  begin(-1);
  expect(!message_tls::clockReady(), "a new sync withdraws the permission earned by the last one");
  expect(message_tls::pollClockSync() == ClockSync::Pending, "never completes");
  message_tls::abortClockSync();
  expect(powerSave == WIFI_PS_MIN_MODEM, "abort restores power-save");
  expect(!message_tls::clockReady(), "abort leaves the permission withdrawn");
  expect(message_tls::pollClockSync() == ClockSync::Failed, "polling after an abort: failed");
  message_tls::abortClockSync();
  expect(powerSave == WIFI_PS_MIN_MODEM && !message_tls::clockReady(), "a second abort does nothing");

  // A sync that completes with an unusable time fails at once, as prepareClock() does, and lets the radio rest.
  for (const int64_t bad : {int64_t{0}, message_tls::MAX_TLS_EPOCH}) {
    clockEpoch = bad;
    const int errorsBefore = errorLogs;
    begin(1);
    expect(message_tls::pollClockSync() == ClockSync::Failed, "completed with an implausible time: failed");
    expect(!message_tls::clockReady(), "no permission for an implausible time");
    expect(powerSave == WIFI_PS_MIN_MODEM, "radio released after a failed sync");
    expect(errorLogs == errorsBefore + 1, "the failure is logged once, as prepareClock() logs it");
  }
  clockEpoch = NOW;

  // Beginning again while one is in flight leaves exactly one guard: the radio comes back to where it was.
  begin(-1);
  begin(2);
  expect(message_tls::pollClockSync() == ClockSync::Pending && message_tls::pollClockSync() == ClockSync::Synced,
         "the second sync is the one that counts");
  expect(powerSave == WIFI_PS_MIN_MODEM, "overlapping begins do not leak the guard");

  // The blocking path is untouched by all of it: it still waits through syncNow() with the radio awake, and a
  // sync left in flight does not change its answer.
  const int syncsBefore = syncCalls;
  begin(-1);
  ntpSuccess = true;
  expect(message_tls::prepareClock() && message_tls::clockReady(),
         "prepareClock still syncs and grants the permission");
  expect(syncCalls == syncsBefore + 1, "through trustedtime::syncNow, once");
  expect(powerSave == WIFI_PS_NONE, "(the abandoned sync's guard is still held)");
  message_tls::abortClockSync();
  expect(powerSave == WIFI_PS_MIN_MODEM, "and an abort releases it");
  expect(message_tls::clockReady(), "without taking away what prepareClock earned");
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  fixtureDirectory = argv[1];
  expect(!message_tls::clockReady(), "cold boot has no current clock permission");
  ntpSuccess = false;
  expect(!message_tls::prepareClock(), "failed sync rejects even a plausible old NVS floor");
  expect(!message_tls::clockReady(), "failed sync cannot retain permission");
  mbedtls_ssl_config config;
  mbedtls_ssl_config_init(&config);
  expect(message_tls::attachDateCheckedBundle(&config) == ESP_FAIL, "clock failure prevents TLS attachment");
  uint32_t deniedFlags = 0;
  expect(config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, nullptr, 0, &deniedFlags) == MBEDTLS_ERR_X509_FATAL_ERROR &&
             (deniedFlags & MBEDTLS_X509_BADCERT_OTHER),
         "clock failure installs denial even if SDK ignores return");
  ntpSuccess = true;
  clockEpoch = 0;
  expect(!message_tls::prepareClock(), "sync success with epoch zero still fails");
  clockEpoch = message_tls::MAX_TLS_EPOCH;
  expect(!message_tls::prepareClock(), "implausibly far-future time fails");
  clockEpoch = NOW;
  expect(message_tls::prepareClock(), "fresh plausible time becomes ready");
  expect(message_tls::clockReady(), "clock permission valid in bounded window");
  uptime += 90000;
  expect(!message_tls::clockReady(), "old sync permission expires");
  uptime = UINT32_MAX - 10;
  expect(message_tls::prepareClock(), "sync near uptime rollover succeeds");
  uptime = 5;
  expect(message_tls::clockReady(), "uptime rollover preserves freshness arithmetic");
  opaqueContext = true;
  expect(message_tls::attachDateCheckedBundle(&config) == ESP_FAIL, "unknown bundle context fails closed");
  deniedFlags = 0;
  expect(config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, nullptr, 0, &deniedFlags) == MBEDTLS_ERR_X509_FATAL_ERROR &&
             (deniedFlags & MBEDTLS_X509_BADCERT_OTHER),
         "unknown context replaces old verifier with denial");
  opaqueContext = false;
  for (auto* state : {&bundleFailure, &missingVerifier, &changedVerifier}) {
    *state = true;
    // Capture the expected callback once before exercising a changed hook.
    if (state == &changedVerifier) {
      *state = false;
      expect(message_tls::attachDateCheckedBundle(&config) == ESP_OK, "normal bundle callback captured");
      *state = true;
    }
    expect(message_tls::attachDateCheckedBundle(&config) == ESP_FAIL, "bundle setup inconsistency rejects attachment");
    deniedFlags = 0;
    expect(config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, nullptr, 0, &deniedFlags) == MBEDTLS_ERR_X509_FATAL_ERROR,
           "bundle setup failure leaves explicit denying callback");
    *state = false;
  }
  expect(message_tls::attachDateCheckedBundle(nullptr) == ESP_FAIL, "missing TLS config fails closed");
  expect(verifyCertificate("expired.pem", false) == 0, "control reproduces installed expired-certificate gap");
  expect(verifyCertificate("future.pem", false) == 0, "control reproduces installed not-before gap");
  expect(verifyCertificate("valid.pem", true) == 0, "valid signed certificate accepted");
  expect(verifyCertificate("expired.pem", true) & MBEDTLS_X509_BADCERT_EXPIRED, "expired leaf rejected");
  expect(verifyCertificate("future.pem", true) & MBEDTLS_X509_BADCERT_FUTURE, "future leaf rejected");
  expect(verifyCertificate("boundary.pem", true) == 0, "inclusive validity boundaries accepted");
  expect(verifyCertificate("expired-intermediate.pem", true) & MBEDTLS_X509_BADCERT_EXPIRED,
         "expired intermediate rejected");
  expect(verifyCertificate("future-intermediate.pem", true) & MBEDTLS_X509_BADCERT_FUTURE,
         "future intermediate rejected");
  expect(verifyCertificate("valid.pem", true, "expired-root.pem") & MBEDTLS_X509_BADCERT_EXPIRED,
         "dated native trust root also checked");
  expect(verifyCertificate("wrong-host.pem", true) & MBEDTLS_X509_BADCERT_CN_MISMATCH, "wrong hostname still rejected");
  expect(verifyCertificate("untrusted.pem", true) & MBEDTLS_X509_BADCERT_NOT_TRUSTED, "untrusted CA still rejected");
  expect(verifyCertificate("bad-signature.der", true) & MBEDTLS_X509_BADCERT_NOT_TRUSTED,
         "bad signature still rejected");
  mbedtls_x509_crt expired;
  parse(expired, "expired.pem");
  expect(message_tls::prepareClock(), "fresh sync before direct callback");
  expect(message_tls::attachDateCheckedBundle(&config) == ESP_OK, "direct callback adapter attaches");
  uint32_t anchorFlags = 0;
  expect(
      config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, const_cast<mbedtls_x509_crt*>(&syntheticAnchor), 1, &anchorFlags) == 0 &&
          anchorFlags == 0,
      "exact synthetic SDK anchor pointer exempted from nonexistent dates");
  mbedtls_x509_crt malformedPeer{};
  expect(config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, &malformedPeer, 0, &anchorFlags) == 0 &&
             (anchorFlags & MBEDTLS_X509_BADCERT_OTHER),
         "identical fields at another peer pointer do not gain anchor exemption");
  uint32_t flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
  clearTrust = true;
  expect(config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, &expired, 0, &flags) == 0 && flags == MBEDTLS_X509_BADCERT_EXPIRED,
         "date flag survives successful bundle trust flag clearing");
  clearTrust = false;
  ntpSuccess = false;
  expect(!message_tls::prepareClock(), "later sync failure invalidates old permission");
  flags = 0;
  expect(config.MBEDTLS_PRIVATE(f_vrfy)(nullptr, &expired, 0, &flags) == 0 && (flags & MBEDTLS_X509_BADCERT_OTHER),
         "callback rejects clock loss during verification");
  const message_tls::UtcDate from{2026, 10, 1, 0, 0, 0}, to{2027, 1, 1, 0, 0, 0};
  expect(message_tls::checkDates(from, to, NOW) == message_tls::DateResult::Valid,
         "UTC date policy accepts current interval");
  expect(message_tls::checkDates({2026, 2, 30, 0, 0, 0}, to, NOW) == message_tls::DateResult::Malformed,
         "invalid calendar date rejected");
  expect(message_tls::checkDates(to, from, NOW) == message_tls::DateResult::Malformed, "reversed interval rejected");
  expect(message_tls::checkDates(from, to, 0) == message_tls::DateResult::InvalidClock,
         "date policy rejects missing clock");
  setenv("TZ", "EST5EDT", 1);
  tzset();
  expect(message_tls::checkDates(from, to, NOW) == message_tls::DateResult::Valid,
         "display timezone cannot shift TLS UTC validation");
  mbedtls_x509_crt_free(&expired);
  mbedtls_ssl_config_free(&config);
  testClockSyncInPieces();
  testRetainedClock();
  testTimeSettingsReachEverySync();
  std::cout << checks << " native TLS checks passed; " << syncCalls << " bounded sync calls\n";
}
