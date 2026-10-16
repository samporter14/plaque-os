#include <Arduino.h>
#include <Logging.h>
#include <TrustedTime.h>
#include <WiFi.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "MethodDevice.h"
#include "WifiCredentialStore.h"
#include "activities/RenderLock.h"
#include "network/DeviceName.h"
#include "network/MessageTls.h"
#include "network/NetworkJoinDevice.h"
#include "network/NetworkProfileDevice.h"

// The reader's side of the Method ports that touch the radio and the clock: the TLS clock (DeviceClock) and Wi-Fi
// (DeviceWifi). They live apart from MethodDevice.cpp (the card, the HTTP client, the hash) so that the host tests
// can build them against a fake Wi-Fi layer (test/network_glue).
namespace method {
namespace {
// Let the radio settle between WiFi.disconnect() and the next begin, as WifiSelectionActivity does.
constexpr uint32_t WIFI_SETTLE_MS = 100;
}  // namespace

// ---- clock ---------------------------------------------------------------------------

uint32_t DeviceClock::nowMs() { return millis(); }

int64_t DeviceClock::unixNow() { return trustedtime::trustedNow(); }

bool DeviceClock::localTime(const int64_t epoch, LocalTime& out) {
  if (epoch <= 0) return false;
  const time_t seconds = static_cast<time_t>(epoch);
  struct tm parts = {};
  if (!localtime_r(&seconds, &parts)) return false;
  out.year = parts.tm_year + 1900;
  out.month = parts.tm_mon + 1;
  out.day = parts.tm_mday;
  out.hour = parts.tm_hour;
  out.minute = parts.tm_min;
  out.weekday = parts.tm_wday;
  out.valid = true;
  return true;
}

bool DeviceClock::clockReady() { return message_tls::clockReady(); }

// The sync runs in message_tls::beginClockSync() / pollClockSync(), the same sync prepareClock() waits for, so the
// shelf polls its input between polls instead of waiting. The 20 s limit is the check's. A poll that has seen the
// sync end is latched here, because the status read behind it is consumed.
void DeviceClock::beginSync() {
  outcome = SyncStatus::Pending;
  message_tls::beginClockSync();  // applies the profile's time servers itself
}

SyncStatus DeviceClock::syncStatus() {
  if (outcome != SyncStatus::Pending) return outcome;
  switch (message_tls::pollClockSync()) {
    case message_tls::ClockSync::Synced:
      outcome = SyncStatus::Synced;
      break;
    case message_tls::ClockSync::Failed:
      outcome = SyncStatus::Failed;
      break;
    case message_tls::ClockSync::Pending:
      break;
  }
  return outcome;
}

void DeviceClock::abortSync() {
  message_tls::abortClockSync();  // a no-op unless a sync is in flight: a finished sync keeps its 90 s window
  outcome = SyncStatus::Pending;
}

// The same fallback Check connection and the Notes poll use: on the clock the reader has kept since an SNTP sync
// recorded in NVS and RTC memory, trusted only within its error bound (clockprov, 10 minutes and 7 days at most).
bool DeviceClock::useRetained() { return message_tls::useRetainedClock(); }

// ---- Wi-Fi ---------------------------------------------------------------------------

int DeviceWifi::savedLastUsed() const {
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (last.empty()) return -1;
  for (size_t i = 0; i < WIFI_STORE.getCredentialCount(); ++i) {
    const auto saved = WIFI_STORE.getSsidAt(i);
    if (saved && *saved == last) return static_cast<int>(i);
  }
  return -1;
}

void DeviceWifi::readSituation() {
  const netprofile::Summary profile = netprofile::summary();
  situation = netjoin::Situation();
  situation.profile = profile.installed;
  situation.profileEapTls = profile.installed && profile.auth == netprofile::Auth::EapTls;
  situation.profileHidden = profile.hidden;
  situation.savedCount = WIFI_STORE.getCredentialCount();
  situation.lastUsed = savedLastUsed();
  profileSsid = profile.installed ? profile.ssid : std::string();
}

bool DeviceWifi::connected() { return WiFi.status() == WL_CONNECTED; }

size_t DeviceWifi::savedCount() {
  readSituation();
  return netjoin::candidateCount(situation);
}

int DeviceWifi::lastUsed() {
  readSituation();
  return netjoin::beforeScan(mode, situation);
}

void DeviceWifi::beginConnect(const size_t saved) {
  connectStep = Step::None;
  // `saved` is numbered by the situation as it is now: the saved networks, then the profile. A caller that numbered
  // the network itself (the connection check's DeviceNetwork) never asked savedCount() or lastUsed(), which are what
  // fill the situation, so without this the profile's index would be taken for a saved network that does not exist.
  readSituation();
  profileAttempt = netjoin::isProfile(situation, saved);
  if (profileAttempt) {
    // The profile's join settles the radio itself, then joins open, PSK or EAP-TLS.
    if (netjoin::beginProfileJoin()) connectStep = Step::Active;
    return;
  }
  const auto credential = WIFI_STORE.getCredentialAt(saved);
  if (!credential) return;  // reads as a failed join
  ssid = credential->ssid;
  password = credential->password;
  // Same sequence as WifiSelectionActivity::attemptConnection, minus its blocking delay.
  WiFi.persistent(false);  // credentials live in WifiCredentialStore, not the SDK's NVS
  devicename::applyStationHostname();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  settleUntil = millis() + WIFI_SETTLE_MS;
  connectStep = Step::Settling;
}

void DeviceWifi::startJoin() {
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  devicename::applyStationHostname();
  if (password.empty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
  connectStep = Step::Active;
}

WifiStatus DeviceWifi::connectStatus() {
  if (connectStep == Step::None) return WifiStatus::Failed;
  if (profileAttempt) {
    switch (netjoin::pollProfileJoin()) {
      case netjoin::Poll::Pending:
        return WifiStatus::Pending;
      case netjoin::Poll::Connected:
        return WifiStatus::Connected;
      case netjoin::Poll::Failed:
        break;
    }
    connectStep = Step::None;
    return WifiStatus::Failed;
  }
  if (connectStep == Step::Settling) {
    if (static_cast<int32_t>(millis() - settleUntil) < 0) return WifiStatus::Pending;
    startJoin();
    return WifiStatus::Pending;
  }
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) return WifiStatus::Connected;
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) return WifiStatus::Failed;
  return WifiStatus::Pending;
}

void DeviceWifi::abortConnect() {
  if (profileAttempt) {
    netjoin::abortProfileJoin();  // disconnects and clears the EAP state
  } else if (connectStep == Step::Active) {
    WiFi.disconnect();
  }
  profileAttempt = false;
  connectStep = Step::None;
  ssid.clear();
  password.clear();
}

void DeviceWifi::beginScan() {
  visible.clear();
  scanSkipped = !netjoin::scans(mode);
  if (scanSkipped) {
    scanStep = Step::None;
    return;
  }
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  settleUntil = millis() + WIFI_SETTLE_MS;
  scanStep = Step::Settling;
}

ScanStatus DeviceWifi::scanStatus() {
  if (scanSkipped) {
    scanSkipped = false;
    visible = netjoin::afterScan(mode, situation, false, {});
    return ScanStatus::Done;
  }
  if (scanStep == Step::None) return ScanStatus::Failed;
  if (scanStep == Step::Settling) {
    if (static_cast<int32_t>(millis() - settleUntil) < 0) return ScanStatus::Pending;
    WiFi.scanNetworks(true);  // asynchronous
    scanStep = Step::Active;
    return ScanStatus::Pending;
  }
  const int16_t found = WiFi.scanComplete();
  if (found == WIFI_SCAN_RUNNING) return ScanStatus::Pending;
  scanStep = Step::None;
  if (found < 0) {
    WiFi.scanDelete();
    return ScanStatus::Failed;
  }
  // The saved networks that are in range, strongest signal first, and whether the profile's network was seen.
  const size_t saved = WIFI_STORE.getCredentialCount();
  std::vector<int32_t> strength(saved, INT32_MIN);
  bool profileSeen = false;
  for (int i = 0; i < found; ++i) {
    const std::string seen = WiFi.SSID(i).c_str();
    if (seen.empty()) continue;
    if (!profileSsid.empty() && seen == profileSsid) profileSeen = true;
    for (size_t j = 0; j < saved; ++j) {
      const auto name = WIFI_STORE.getSsidAt(j);
      if (name && *name == seen && WiFi.RSSI(i) > strength[j]) strength[j] = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();
  std::vector<size_t> savedSeen;
  savedSeen.reserve(saved);
  for (size_t j = 0; j < saved; ++j)
    if (strength[j] != INT32_MIN) savedSeen.push_back(j);
  std::stable_sort(savedSeen.begin(), savedSeen.end(), [&](size_t a, size_t b) { return strength[a] > strength[b]; });
  visible = netjoin::afterScan(mode, situation, profileSeen, savedSeen);
  return ScanStatus::Done;
}

void DeviceWifi::noteJoined(const size_t saved) {
  // The profile is always tried first anyway; the saved networks' own order stays as it was. Which of the two joined
  // is what the next timer wake tries first (netjoin::lastJoined()).
  if (netjoin::isProfile(situation, saved)) {
    netjoin::rememberJoined(netjoin::Joined::Profile);
    return;
  }
  netjoin::rememberJoined(netjoin::Joined::Saved);
  // Remember the network that worked so the next check tries it first; the store saves only on a change.
  const auto name = WIFI_STORE.getSsidAt(saved);
  if (!name) return;
  pendingNote = *name;
  writePendingNote();
}

// The write takes the lock WifiSelectionActivity holds around it, but only tries for it: waiting would sit out a
// panel refresh with no input read. While the render task has the lock the name stays pending and the shelf asks
// again on its next pass; a note still pending when the check is torn down is dropped (the next check scans once).
void DeviceWifi::writePendingNote() {
  if (pendingNote.empty()) return;
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return;
  WIFI_STORE.setLastConnectedSsid(pendingNote);
  pendingNote.clear();
}

void DeviceWifi::off() {
  pendingNote.clear();
  connectStep = Step::None;
  scanStep = Step::None;
  profileAttempt = false;
  scanSkipped = false;
  ssid.clear();
  password.clear();
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.scanDelete();
  ambient::disconnectWifi();  // clears a profile's EAP state before the radio goes off
}

}  // namespace method
