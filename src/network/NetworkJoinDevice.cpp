#include "NetworkJoinDevice.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_eap_client.h>
#include <esp_wifi.h>
#include <mbedtls/build_info.h>

#include <atomic>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "NetCodec.h"
#include "NetworkJoin.h"
#include "NetworkProfileDevice.h"
#include "WifiCredentialStore.h"
#include "network/DeviceName.h"
#include "network/MessageTls.h"

namespace netjoin {
namespace {

// Let the radio settle between WiFi.disconnect() and the next begin, as WifiSelectionActivity does.
constexpr uint32_t SETTLE_MS = 100;
// How often a wake's blocking join looks at the radio. Each candidate's budget is in NetworkJoin.h; steptimer::
// pollBudgetMs() takes their sum, so a step timer's alert is never late because of the join.
constexpr uint32_t WAKE_POLL_MS = 50;

// Who joined last (netjoin::Joined, encoded): RTC memory keeps it through deep sleep. A power-off or a crash clears
// it to zero or leaves something that does not decode, which reads as nobody.
RTC_DATA_ATTR uint8_t joinedMemory = 0;

enum class Step : uint8_t { None, Settling, Active };

// What the supplicant reads while the station is on an EAP-TLS network. It keeps the pointers, not copies.
struct Enterprise {
  netprofile::SecretBytes caPem;  // the CA chain, NUL included in size()
  netprofile::SecretBytes certPem;
  netprofile::SecretBytes keyPem;
};

struct ProfileJoin {
  Step step = Step::None;
  uint32_t settleUntil = 0;
  netprofile::Profile profile;  // secrets, only while a join is being made
};

ProfileJoin current;
std::unique_ptr<Enterprise> enterprise;
bool enterpriseOn = false;
bool viaProfile = false;
std::atomic<uint8_t> disconnectReason{0};
bool eventHooked = false;

void onStationEvent(arduino_event_t* event) {
  if (event && event->event_id == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    disconnectReason.store(static_cast<uint8_t>(event->event_info.wifi_sta_disconnected.reason));
}

const char* keyLabel(const netprofile::KeyFormat format) {
  switch (format) {
    case netprofile::KeyFormat::Pkcs1:
      return "RSA PRIVATE KEY";
    case netprofile::KeyFormat::Sec1:
      return "EC PRIVATE KEY";
    case netprofile::KeyFormat::Pkcs8:
      break;
  }
  return "PRIVATE KEY";
}

// DER blocks as PEM, back to back, with the terminating NUL counted in size(): the supplicant parses a PEM blob
// only when its length includes the NUL, as the embedded files of the ESP-IDF example do.
bool pemOf(const char* label, const uint8_t* der, const std::vector<std::pair<size_t, size_t>>& parts,
           netprofile::SecretBytes& out) {
  size_t total = 1;
  for (const auto& part : parts) total += netcodec::pemSize(label, part.second);
  if (!out.allocate(total)) return false;
  size_t written = 0;
  for (const auto& part : parts)
    written += netcodec::pemWrite(label, der + part.first, part.second, reinterpret_cast<char*>(out.data()) + written);
  out.data()[written] = 0;
  return written + 1 == total;
}

std::unique_ptr<Enterprise> buildEnterprise(const netprofile::Profile& p) {
  auto e = makeUniqueNoThrow<Enterprise>();
  if (!e) return nullptr;
  std::vector<std::pair<size_t, size_t>> ca;
  if (!netprofile::splitDer(p.caDer, ca) || !pemOf("CERTIFICATE", p.caDer.data(), ca, e->caPem)) return nullptr;
  const std::vector<std::pair<size_t, size_t>> cert = {{0, p.clientCertDer.size()}};
  const std::vector<std::pair<size_t, size_t>> key = {{0, p.keyDer.size()}};
  if (!pemOf("CERTIFICATE", p.clientCertDer.data(), cert, e->certPem) ||
      !pemOf(keyLabel(p.keyFormat), p.keyDer.data(), key, e->keyPem))
    return nullptr;
  return e;
}

// Hands the profile's EAP-TLS settings to the supplicant. Only EAP-TLS, only the profile's CA (never the public
// bundle), the server name it must present, and the date check while the clock is trusted.
bool enableEnterprise(const netprofile::Profile& p) {
  releaseEnterprise();
  enterprise = buildEnterprise(p);
  if (!enterprise) {
    LOG_ERR("NETJ", "OOM or bad material building the EAP-TLS buffers");
    return false;
  }
  const bool ok =
      esp_eap_client_set_eap_methods(ESP_EAP_TYPE_TLS) == ESP_OK &&
      esp_eap_client_use_default_cert_bundle(false) == ESP_OK &&
      esp_eap_client_set_identity(reinterpret_cast<const unsigned char*>(p.identity.data()),
                                  static_cast<int>(p.identity.size())) == ESP_OK &&
      esp_eap_client_set_ca_cert(enterprise->caPem.data(), static_cast<int>(enterprise->caPem.size())) == ESP_OK &&
      esp_eap_client_set_certificate_and_key(enterprise->certPem.data(), static_cast<int>(enterprise->certPem.size()),
                                             enterprise->keyPem.data(), static_cast<int>(enterprise->keyPem.size()),
                                             nullptr, 0) == ESP_OK &&
      esp_eap_client_set_domain_name(p.info.serverDomain.c_str()) == ESP_OK &&
      esp_eap_client_set_disable_time_check(!eapClockTrusted()) == ESP_OK;
  enterpriseOn = true;  // from here on there is EAP state to clear, whatever enable() says
  if (!ok || esp_wifi_sta_enterprise_enable() != ESP_OK) {
    LOG_ERR("NETJ", "Could not configure EAP-TLS");
    releaseEnterprise();
    return false;
  }
  LOG_INF("NETJ", "EAP-TLS configured (CA %u B, certificate %u B, key %u B; dates %s)",
          static_cast<unsigned>(enterprise->caPem.size()), static_cast<unsigned>(enterprise->certPem.size()),
          static_cast<unsigned>(enterprise->keyPem.size()), eapServerDatesChecked() ? "checked" : "not checked");
  return true;
}

// The network the station joined uses the authentication the profile asks for.
bool authMatches(const netprofile::Auth auth) {
  wifi_ap_record_t ap = {};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return auth == netprofile::Auth::Open;
  switch (auth) {
    case netprofile::Auth::EapTls:
      return ap.authmode == WIFI_AUTH_WPA2_ENTERPRISE || ap.authmode == WIFI_AUTH_WPA3_ENTERPRISE ||
             ap.authmode == WIFI_AUTH_WPA2_WPA3_ENTERPRISE || ap.authmode == WIFI_AUTH_WPA3_ENT_192 ||
             ap.authmode == WIFI_AUTH_WPA_ENTERPRISE;
    case netprofile::Auth::Psk:
      return ap.authmode != WIFI_AUTH_OPEN && ap.authmode != WIFI_AUTH_OWE;
    case netprofile::Auth::Open:
      break;
  }
  return true;
}

// WiFi.begin(ssid) without a passphrase leaves the scan's authentication threshold at "open": the station takes the
// strongest signal under that name, an open or PSK network of the same name included. The Arduino core sets the
// threshold only when a passphrase is given, so for EAP-TLS it is set here, between the core's begin (which makes the
// config but does not connect) and the connect: only WPA2-Enterprise or stronger can be joined. (A WPA3-PSK network of
// the same name ranks above WPA2-Enterprise in the driver's order, if not by kind; authMatches() catches it after the
// association, as it did before.)
bool beginEnterpriseOnly(const std::string& ssid) {
  WiFi.begin(ssid.c_str(), nullptr, 0, nullptr, false);
  wifi_config_t config = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK) {
    LOG_ERR("NETJ", "Could not read the station configuration");
    return false;
  }
  config.sta.threshold.authmode = WIFI_AUTH_WPA2_ENTERPRISE;
  if (esp_wifi_set_config(WIFI_IF_STA, &config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
    LOG_ERR("NETJ", "Could not start the EAP-TLS join");
    return false;
  }
  return true;
}

void startProfileJoin() {
  const netprofile::Profile& p = current.profile;
  if (!eventHooked) {
    WiFi.onEvent(onStationEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    eventHooked = true;
  }
  disconnectReason.store(0);
  WiFi.persistent(false);  // the profile lives in its own NVS namespace, never in the driver's
  devicename::applyStationHostname();
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);  // a directed probe: a hidden network answers to its name
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (p.info.auth == netprofile::Auth::EapTls && !enableEnterprise(p)) {
    current.step = Step::None;
    return;
  }
  if (p.info.auth == netprofile::Auth::Psk) {
    WiFi.begin(p.info.ssid.c_str(), p.psk.c_str());  // the core's minimum security (WPA2-PSK) applies with a passphrase
  } else if (p.info.auth == netprofile::Auth::EapTls) {
    if (!beginEnterpriseOnly(p.info.ssid)) {
      current.step = Step::None;
      abortProfileJoin();
      return;
    }
  } else {
    WiFi.begin(p.info.ssid.c_str());
  }
  current.step = Step::Active;
}

void forgetProfile() {
  current.step = Step::None;
  current.profile = netprofile::Profile();  // wipes the PSK and the key copy
}

}  // namespace

bool eapClockTrusted() { return message_tls::certificateClockTrusted(); }

bool eapServerDatesChecked() {
#if defined(MBEDTLS_HAVE_TIME_DATE)
  return eapClockTrusted();
#else
  return false;
#endif
}

uint8_t lastDisconnectReason() { return disconnectReason.load(); }

bool joinedViaProfile() { return viaProfile && WiFi.status() == WL_CONNECTED; }

void releaseEnterprise() {
  if (enterpriseOn) {
    // Disable first: the supplicant stops using the buffers before they are cleared and freed.
    esp_wifi_sta_enterprise_disable();
    esp_eap_client_clear_identity();
    esp_eap_client_clear_ca_cert();
    esp_eap_client_clear_certificate_and_key();
    esp_eap_client_set_domain_name(nullptr);
    esp_eap_client_set_eap_methods(ESP_EAP_TYPE_ALL);  // back to the supplicant's default (enableEnterprise() set TLS)
    enterpriseOn = false;
  }
  enterprise.reset();  // SecretBytes zero themselves
  viaProfile = false;
}

bool beginProfileJoin() {
  abortProfileJoin();
  if (!netprofile::installed()) {  // nothing to join: not an error, and not worth a log line at every check
    forgetProfile();
    return false;
  }
  if (!netprofile::loadForJoin(current.profile)) {
    LOG_ERR("NETJ", "No usable network profile to join");
    forgetProfile();
    return false;
  }
  // Before the join, not after: lwIP takes an NTP server from the DHCP lease as it arrives (TrustedTime.h).
  netprofile::applyDhcpTimeSetting();
  // Same sequence as a saved network (DeviceWifi::beginConnect), minus its blocking delay.
  WiFi.persistent(false);
  devicename::applyStationHostname();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  current.settleUntil = millis() + SETTLE_MS;
  current.step = Step::Settling;
  LOG_INF("NETJ", "Joining the network profile (%s%s)", netprofile::authName(current.profile.info.auth),
          current.profile.info.hidden ? ", hidden" : "");
  return true;
}

Poll pollProfileJoin() {
  if (current.step == Step::None) return Poll::Failed;
  if (current.step == Step::Settling) {
    if (static_cast<int32_t>(millis() - current.settleUntil) < 0) return Poll::Pending;
    startProfileJoin();
    return current.step == Step::Active ? Poll::Pending : Poll::Failed;
  }
  const wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    if (!authMatches(current.profile.info.auth)) {
      LOG_ERR("NETJ", "Joined a network named like the profile without its authentication: leaving it");
      abortProfileJoin();
      return Poll::Failed;
    }
    // Joined: the PSK and the key copy go; what EAP needs stays in `enterprise` until the radio goes off.
    LOG_INF("NETJ", "Joined the network profile");
    forgetProfile();
    viaProfile = true;
    rememberJoined(Joined::Profile);
    return Poll::Connected;
  }
  if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
    LOG_INF("NETJ", "Profile join failed (status %d, reason %u)", static_cast<int>(status),
            static_cast<unsigned>(disconnectReason.load()));
    abortProfileJoin();
    return Poll::Failed;
  }
  return Poll::Pending;
}

void abortProfileJoin() {
  // Leave the network first (one being joined, or one joined through the profile), then clear the EAP state.
  if (current.step == Step::Active || viaProfile) WiFi.disconnect();
  viaProfile = false;
  forgetProfile();
  releaseEnterprise();
}

namespace {
// The profile and the saved networks as a wake sees them (the saved-network store is read as it stands).
Situation wakeSituation() {
  const netprofile::Summary profile = netprofile::summary();
  Situation situation;
  situation.profile = profile.installed;
  situation.profileEapTls = profile.installed && profile.auth == netprofile::Auth::EapTls;
  situation.profileHidden = profile.hidden;
  situation.savedCount = WIFI_STORE.getCredentialCount();
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  for (size_t i = 0; !last.empty() && i < situation.savedCount; ++i) {
    const auto ssid = WIFI_STORE.getSsidAt(i);
    if (ssid && *ssid == last) {
      situation.lastUsed = static_cast<int>(i);
      break;
    }
  }
  return situation;
}
}  // namespace

bool hasWakeCandidate() { return !wakeOrder(wakeSituation()).empty(); }

Joined lastJoined() { return decodeJoined(joinedMemory); }

void rememberJoined(const Joined joined) { joinedMemory = encodeJoined(joined); }

uint32_t wakeJoinWorstMsNow() {
  const netprofile::Summary profile = netprofile::summary();
  return wakeJoinWorstMs(profile.installed, profile.installed && profile.auth == netprofile::Auth::EapTls);
}

bool joinForWake() {
  if (WiFi.status() == WL_CONNECTED) return true;
  WIFI_STORE.loadFromFile();
  netprofile::applyDhcpTimeSetting();  // before any join: the lease is where the server comes from
  const Situation situation = wakeSituation();
  // Each candidate has a budget of its own and the one that joined last goes first (NetworkJoin.h): a slow or absent
  // profile no longer takes the last-used network's time, and a profile that needs an EAP-TLS handshake gets enough.
  for (const WakeStep& step : wakePlan(situation, lastJoined())) {
    const uint32_t deadline = millis() + step.budgetMs;
    const auto timeLeft = [&] { return static_cast<int32_t>(deadline - millis()) > 0; };
    if (isProfile(situation, step.candidate)) {
      if (!beginProfileJoin()) continue;
      Poll poll = Poll::Pending;
      while (timeLeft() && (poll = pollProfileJoin()) == Poll::Pending) delay(WAKE_POLL_MS);
      if (poll == Poll::Connected) return true;  // pollProfileJoin() remembered it
      abortProfileJoin();
      LOG_INF("NETJ", "Wake: the profile did not join, trying the last-used network");
      continue;
    }
    const auto credential = WIFI_STORE.getCredentialAt(step.candidate);
    if (!credential) continue;
    viaProfile = false;
    devicename::applyStationHostname();
    WiFi.mode(WIFI_STA);
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    while (timeLeft() && WiFi.status() != WL_CONNECTED) delay(WAKE_POLL_MS);
    if (WiFi.status() == WL_CONNECTED) {
      rememberJoined(Joined::Saved);
      return true;
    }
    WiFi.disconnect();
  }
  return WiFi.status() == WL_CONNECTED;
}

}  // namespace netjoin

// The two calls the ambient code makes (declared in ambient/LatestMessageProvider.h). They live here, beside the EAP
// state they release, so that the host tests build the same code.
namespace ambient {
bool connectSavedWifi() { return netjoin::joinForWake(); }

void disconnectWifi() {
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.disconnect();
  // While the driver still runs: a later open or PSK join must not find EAP settings left over.
  netjoin::releaseEnterprise();
  if (WiFi.getMode() != WIFI_MODE_NULL) WiFi.mode(WIFI_OFF);
}
}  // namespace ambient
