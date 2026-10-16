// The fake reader behind the glue tests: virtual time, a Wi-Fi radio with access points in range, the supplicant's EAP
// state, NVS, the card, the saved-network store, the clock and the log. The device code under test (NetworkJoinDevice,
// NetworkProfileDevice, MethodNetDevice, ConnectionCheckNetwork) is compiled unchanged against these.
#include <Arduino.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Preferences.h>
#include <TrustedTime.h>
#include <WiFi.h>
#include <WifiCredentialStore.h>
#include <esp_crt_bundle.h>
#include <esp_eap_client.h>
#include <esp_random.h>
#include <esp_rtc_time.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <sys/time.h>
#undef gettimeofday

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "FakeWorld.h"
#include "activities/RenderLock.h"
#include "network/DeviceName.h"
#include "network/MessageTls.h"

namespace {
void (*syncCallback)(struct timeval*) = nullptr;  // sntp_set_time_sync_notification_cb()
}

namespace fake {

World& world() {
  static World instance;
  return instance;
}

void reset() { world() = World(); }

namespace {

// The IDF's order of strength (esp_wifi_types_generic.h): personal modes below enterprise ones.
int strength(const wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return 0;
    case WIFI_AUTH_WEP:
      return 1;
    case WIFI_AUTH_WPA_PSK:
      return 2;
    case WIFI_AUTH_OWE:
      return 3;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
      return 4;
    case WIFI_AUTH_WAPI_PSK:
      return 5;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_DPP:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
      return 6;
    case WIFI_AUTH_WPA_ENTERPRISE:
      return 10;
    case WIFI_AUTH_WPA2_ENTERPRISE:  // = WIFI_AUTH_ENTERPRISE
      return 11;
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
      return 12;
    case WIFI_AUTH_WPA3_ENT_192:
      return 13;
    case WIFI_AUTH_MAX:
      break;
  }
  return 0;
}

bool isEnterprise(const wifi_auth_mode_t mode) { return strength(mode) >= 10; }

void fire(const arduino_event_id_t id, const arduino_event_info_t& info) {
  arduino_event_t event;
  event.event_id = id;
  event.event_info = info;
  const std::vector<Radio::Handler> handlers = world().radio.handlers;  // a handler may change the list
  for (const auto& handler : handlers)
    if (handler.filter == ARDUINO_EVENT_MAX || handler.filter == id) handler.callback(&event);
}

void fireDisconnected(const int reason) {
  arduino_event_info_t info = {};
  info.wifi_sta_disconnected.reason = static_cast<uint8_t>(reason);
  fire(ARDUINO_EVENT_WIFI_STA_DISCONNECTED, info);
}

void settle() {
  Radio& r = world().radio;
  if (r.outcomeIsFailure) {
    r.state = Radio::State::Failed;
    fireDisconnected(r.failReason);
    return;
  }
  const AccessPoint& ap = r.aps[static_cast<size_t>(r.target)];
  r.state = Radio::State::Connected;
  r.joinedAuth = ap.auth;
  r.joinedSsid = ap.ssid;
  arduino_event_info_t info = {};
  info.wifi_sta_connected.channel = static_cast<uint8_t>(ap.channel);
  info.wifi_sta_connected.authmode = ap.auth;
  fire(ARDUINO_EVENT_WIFI_STA_CONNECTED, info);
}

void update() {
  World& w = world();
  Radio& r = w.radio;
  if (r.state == Radio::State::Connecting && static_cast<int32_t>(w.now - r.settledAt) >= 0) settle();
  if (r.scanning && static_cast<int32_t>(w.now - r.scanDoneAt) >= 0) {
    r.scanning = false;
    r.scanResult.clear();
    for (const auto& ap : r.aps)
      if (!ap.hidden) r.scanResult.push_back(ap);
    std::stable_sort(r.scanResult.begin(), r.scanResult.end(),
                     [](const AccessPoint& a, const AccessPoint& b) { return a.rssi > b.rssi; });
  }
}

}  // namespace

// The SNTP client's side of the clock: once ntpDelayMs have passed since it was (re)started, a sync completes.
void sntpStep() {
  Clock& c = world().clock;
  if (!c.sntpEnabled || c.sntpFired || !c.ntpAnswers) return;
  if (static_cast<uint32_t>(world().now - c.sntpStartedAt) < c.ntpDelayMs) return;
  c.sntpFired = true;
  c.timeEpoch = c.ntpEpoch;
  c.sntpStatusPending = true;
  if (syncCallback) syncCallback(nullptr);
}

void advance(const uint32_t ms) {
  world().now += ms;
  update();
  sntpStep();
}

void deliverLeaseNtpServer(const unsigned address) {
  Clock& c = world().clock;
  if (!c.dhcpMode) return;
  c.slotAddress[0] = address;
  c.slotNamed[0] = false;
}

int logCount(const std::string& text) {
  int count = 0;
  for (const auto& line : world().log)
    if (line.find(text) != std::string::npos) ++count;
  return count;
}

}  // namespace fake

// ---- time and log
// -----------------------------------------------------------------------------------------------------

unsigned long millis() { return fake::world().now; }

void delay(const uint32_t ms) { fake::advance(ms); }

void gluelog(const int level, const char* tag, const char* format, ...) {
  char text[400];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  fake::World& w = fake::world();
  if (level == 2) ++w.errorLogs;
  w.log.push_back(std::string(level == 2 ? "E " : "I ") + tag + ": " + text);
}

// ---- Wi-Fi (Arduino core)
// ---------------------------------------------------------------------------------------------

WiFiClass WiFi;

void WiFiClass::persistent(bool) {}

bool WiFiClass::mode(const wifi_mode_t newMode) {
  fake::Radio& r = fake::world().radio;
  if (newMode == WIFI_MODE_NULL) {
    ++r.modeOffs;
    r.state = fake::Radio::State::Idle;
    r.scanning = false;
  }
  r.mode = newMode;
  return true;
}

wifi_mode_t WiFiClass::getMode() { return fake::world().radio.mode; }

bool WiFiClass::disconnect(const bool wifioff, bool) {
  fake::Radio& r = fake::world().radio;
  ++r.disconnects;
  const bool was = r.state == fake::Radio::State::Connected || r.state == fake::Radio::State::Connecting;
  r.state = fake::Radio::State::Idle;
  if (was) fake::fireDisconnected(WIFI_REASON_ASSOC_LEAVE);
  if (wifioff) mode(WIFI_MODE_NULL);
  return true;
}

wl_status_t WiFiClass::begin(const char* ssid, const char* passphrase, int32_t, const uint8_t*, const bool connect) {
  fake::Radio& r = fake::world().radio;
  if (r.mode == WIFI_MODE_NULL) r.mode = WIFI_MODE_STA;  // the core's STA.begin()
  wifi_config_t conf = {};
  std::strncpy(reinterpret_cast<char*>(conf.sta.ssid), ssid, sizeof(conf.sta.ssid));
  const bool hasPassphrase = passphrase != nullptr && passphrase[0] != 0;
  if (hasPassphrase) {
    conf.sta.threshold.authmode = r.minSecurity;  // STA.cpp sets the threshold only when there is a passphrase
    std::strncpy(reinterpret_cast<char*>(conf.sta.password), passphrase, sizeof(conf.sta.password));
  }
  r.config = conf;
  fake::world().clock.trail.push_back("wifiBegin");
  fake::Radio::Begin entry;
  entry.ssid = ssid;
  entry.hadPassphrase = hasPassphrase;
  if (hasPassphrase) entry.passphrase = passphrase;
  entry.connectNow = connect;
  r.begins.push_back(entry);
  if (connect) esp_wifi_connect();
  return status();
}

wl_status_t WiFiClass::status() {
  fake::update();
  const fake::Radio& r = fake::world().radio;
  if (r.mode == WIFI_MODE_NULL) return WL_NO_SHIELD;
  switch (r.state) {
    case fake::Radio::State::Connected:
      return WL_CONNECTED;
    case fake::Radio::State::Failed:
      return r.failStatus;
    case fake::Radio::State::Connecting:
    case fake::Radio::State::Idle:
      break;
  }
  return WL_DISCONNECTED;
}

void WiFiClass::setScanMethod(wifi_scan_method_t) {}
void WiFiClass::setSortMethod(wifi_sort_method_t) {}
void WiFiClass::setMinSecurity(const wifi_auth_mode_t minSecurity) { fake::world().radio.minSecurity = minSecurity; }

network_event_handle_t WiFiClass::onEvent(const NetworkEventSysCb callback, const arduino_event_id_t event) {
  fake::Radio& r = fake::world().radio;
  const size_t handle = r.nextHandle++;
  r.handlers.push_back({callback, event, handle});
  return handle;
}

bool WiFiClass::removeEvent(const network_event_handle_t handle) {
  auto& handlers = fake::world().radio.handlers;
  const auto found = std::find_if(handlers.begin(), handlers.end(), [&](const auto& h) { return h.handle == handle; });
  if (found == handlers.end()) return false;
  handlers.erase(found);
  return true;
}

int16_t WiFiClass::scanNetworks(bool) {
  fake::World& w = fake::world();
  w.radio.scanning = true;
  w.radio.scanDoneAt = w.now + 2000;
  w.radio.scanResult.clear();
  return WIFI_SCAN_RUNNING;
}

int16_t WiFiClass::scanComplete() {
  fake::update();
  const fake::Radio& r = fake::world().radio;
  if (r.scanning) return WIFI_SCAN_RUNNING;
  return static_cast<int16_t>(r.scanResult.size());
}

void WiFiClass::scanDelete() { fake::world().radio.scanResult.clear(); }

WiFiClass::SsidText WiFiClass::SSID(const uint8_t index) {
  const auto& found = fake::world().radio.scanResult;
  return {index < found.size() ? found[index].ssid : std::string()};
}

int32_t WiFiClass::RSSI(const uint8_t index) {
  const auto& found = fake::world().radio.scanResult;
  return index < found.size() ? found[index].rssi : 0;
}

int8_t WiFiClass::RSSI() { return fake::world().radio.state == fake::Radio::State::Connected ? -55 : 0; }
uint8_t WiFiClass::channel() { return 6; }

IPAddress WiFiClass::localIP() {
  const fake::Radio& r = fake::world().radio;
  return r.state == fake::Radio::State::Connected && r.leaseGiven ? IPAddress(10, 1, 2, 3) : IPAddress();
}
IPAddress WiFiClass::gatewayIP() {
  const fake::Radio& r = fake::world().radio;
  return r.state == fake::Radio::State::Connected && r.leaseGiven ? IPAddress(10, 1, 0, 1) : IPAddress();
}
IPAddress WiFiClass::dnsIP(const uint8_t index) {
  const fake::Radio& r = fake::world().radio;
  return r.state == fake::Radio::State::Connected && r.leaseGiven && index == 0 ? IPAddress(10, 0, 0, 53) : IPAddress();
}

// ---- Wi-Fi (ESP-IDF driver)
// -------------------------------------------------------------------------------------------

esp_err_t esp_wifi_get_config(wifi_interface_t, wifi_config_t* conf) {
  *conf = fake::world().radio.config;
  return ESP_OK;
}

esp_err_t esp_wifi_set_config(wifi_interface_t, wifi_config_t* conf) {
  fake::world().radio.config = *conf;
  return ESP_OK;
}

// The attempt: which access points the config could use, which one it goes to, and how it ends. The auth threshold
// keeps weaker networks out of the candidates (a PSK or open twin of an enterprise network); among the rest the
// strongest signal wins.
esp_err_t esp_wifi_connect() {
  fake::World& w = fake::world();
  fake::Radio& r = w.radio;
  ++r.connectCalls;
  r.state = fake::Radio::State::Connecting;
  r.connectStartedAt = w.now;
  const std::string ssid(reinterpret_cast<const char*>(r.config.sta.ssid));
  const std::string password(reinterpret_cast<const char*>(r.config.sta.password));
  const wifi_auth_mode_t threshold = r.config.sta.threshold.authmode;

  std::vector<int> candidates;
  bool sawName = false;
  for (size_t i = 0; i < r.aps.size(); ++i) {
    if (r.aps[i].ssid != ssid) continue;
    sawName = true;
    if (fake::strength(r.aps[i].auth) >= fake::strength(threshold)) candidates.push_back(static_cast<int>(i));
  }
  std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) {
    return r.aps[static_cast<size_t>(a)].rssi > r.aps[static_cast<size_t>(b)].rssi;
  });
  fake::Radio::Attempt attempt;
  attempt.ssid = ssid;
  attempt.threshold = threshold;
  attempt.enterpriseEnabled = w.eap.enterpriseEnabled;
  attempt.target = candidates.empty() ? -1 : candidates.front();
  r.attempts.push_back(attempt);

  r.target = attempt.target;
  if (candidates.empty()) {
    r.outcomeIsFailure = true;
    r.failStatus = WL_NO_SSID_AVAIL;
    r.failReason = sawName ? WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD : WIFI_REASON_NO_AP_FOUND;
    r.settledAt = w.now + 300;
    return ESP_OK;
  }
  const fake::AccessPoint& ap = r.aps[static_cast<size_t>(r.target)];
  bool ok = false;
  if (ap.auth == WIFI_AUTH_OPEN) {
    ok = true;
  } else if (fake::isEnterprise(ap.auth)) {
    const fake::Eap& eap = w.eap;
    ok = eap.enterpriseEnabled && eap.identitySet && eap.caSet && eap.certSet && eap.keySet && ap.radiusAccepts &&
         (ap.radiusDomain.empty() || ap.radiusDomain == eap.domain);
  } else {
    ok = password == ap.psk;
  }
  r.outcomeIsFailure = !ok;
  r.failStatus = WL_CONNECT_FAILED;
  r.failReason = WIFI_REASON_AUTH_FAIL;
  r.settledAt = w.now + (ok ? ap.joinMs : 800);
  return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap) {
  const fake::Radio& r = fake::world().radio;
  if (r.state != fake::Radio::State::Connected) return ESP_FAIL;
  *ap = {};
  std::strncpy(reinterpret_cast<char*>(ap->ssid), r.joinedSsid.c_str(), sizeof(ap->ssid) - 1);
  ap->authmode = r.joinedAuth;
  ap->rssi = -55;
  return ESP_OK;
}

esp_err_t esp_wifi_sta_enterprise_enable() {
  fake::world().eap.enterpriseEnabled = true;
  ++fake::world().eap.enableCalls;
  return ESP_OK;
}

esp_err_t esp_wifi_sta_enterprise_disable() {
  fake::world().eap.enterpriseEnabled = false;
  ++fake::world().eap.disableCalls;
  return ESP_OK;
}

// ---- the supplicant
// ---------------------------------------------------------------------------------------------------

esp_err_t esp_eap_client_set_eap_methods(const esp_eap_method_t methods) {
  if ((methods & ~ESP_EAP_TYPE_ALL) != 0 || methods == 0) return ESP_ERR_INVALID_ARG;
  fake::world().eap.methods = methods;
  return ESP_OK;
}

esp_err_t esp_eap_client_use_default_cert_bundle(const bool use) {
  fake::world().eap.useDefaultBundle = use;
  return ESP_OK;
}

esp_err_t esp_eap_client_set_identity(const unsigned char* identity, const int len) {
  if (identity == nullptr || len < 1 || len > 127) return ESP_ERR_INVALID_ARG;
  fake::Eap& eap = fake::world().eap;
  eap.identitySet = true;
  eap.identity.assign(reinterpret_cast<const char*>(identity), static_cast<size_t>(len));
  return ESP_OK;
}

esp_err_t esp_eap_client_set_ca_cert(const unsigned char* ca, const int len) {
  if (ca == nullptr || len < 1) return ESP_ERR_INVALID_ARG;
  fake::world().eap.caSet = true;
  return ESP_OK;
}

esp_err_t esp_eap_client_set_certificate_and_key(const unsigned char* cert, const int certLen, const unsigned char* key,
                                                 const int keyLen, const unsigned char*, const int) {
  if (cert == nullptr || certLen < 1 || key == nullptr || keyLen < 1) return ESP_ERR_INVALID_ARG;
  fake::world().eap.certSet = true;
  fake::world().eap.keySet = true;
  return ESP_OK;
}

esp_err_t esp_eap_client_set_domain_name(const char* domain) {
  fake::world().eap.domain = domain ? domain : "";
  return ESP_OK;
}

esp_err_t esp_eap_client_set_disable_time_check(const bool disable) {
  fake::world().eap.disableTimeCheck = disable;
  return ESP_OK;
}

void esp_eap_client_clear_identity() {
  fake::world().eap.identitySet = false;
  fake::world().eap.identity.clear();
  ++fake::world().eap.clearCalls;
}

void esp_eap_client_clear_ca_cert() {
  fake::world().eap.caSet = false;
  ++fake::world().eap.clearCalls;
}

void esp_eap_client_clear_certificate_and_key() {
  fake::world().eap.certSet = false;
  fake::world().eap.keySet = false;
  ++fake::world().eap.clearCalls;
}

// ---- NVS
// --------------------------------------------------------------------------------------------------------------

namespace {
struct NvsHandle {
  std::string name;
  bool writable = false;
};
std::vector<NvsHandle>& nvsHandles() {
  static std::vector<NvsHandle> handles;
  return handles;
}
std::map<std::string, std::vector<uint8_t>>& nvsNamespace(const nvs_handle_t handle) {
  return fake::world().nvs.data[nvsHandles()[handle - 1].name];
}
}  // namespace

esp_err_t nvs_open(const char* name, const nvs_open_mode_t mode, nvs_handle_t* handle) {
  auto& data = fake::world().nvs.data;
  if (mode == NVS_READONLY && data.find(name) == data.end()) return ESP_ERR_NVS_NOT_FOUND;
  data[name];
  nvsHandles().push_back({name, mode == NVS_READWRITE});
  *handle = static_cast<nvs_handle_t>(nvsHandles().size());
  return ESP_OK;
}

void nvs_close(nvs_handle_t) {}

esp_err_t nvs_get_u8(const nvs_handle_t handle, const char* key, uint8_t* value) {
  auto& space = nvsNamespace(handle);
  const auto found = space.find(key);
  if (found == space.end()) return ESP_ERR_NVS_NOT_FOUND;
  *value = found->second.empty() ? 0 : found->second[0];
  return ESP_OK;
}

esp_err_t nvs_set_u8(const nvs_handle_t handle, const char* key, const uint8_t value) {
  nvsNamespace(handle)[key] = {value};
  return ESP_OK;
}

esp_err_t nvs_get_blob(const nvs_handle_t handle, const char* key, void* out, size_t* length) {
  auto& space = nvsNamespace(handle);
  const auto found = space.find(key);
  if (found == space.end()) return ESP_ERR_NVS_NOT_FOUND;
  if (out == nullptr) {
    *length = found->second.size();
    return ESP_OK;
  }
  if (*length < found->second.size()) return ESP_ERR_NVS_INVALID_LENGTH;
  std::memcpy(out, found->second.data(), found->second.size());
  *length = found->second.size();
  return ESP_OK;
}

esp_err_t nvs_set_blob(const nvs_handle_t handle, const char* key, const void* value, const size_t length) {
  const auto* bytes = static_cast<const uint8_t*>(value);
  nvsNamespace(handle)[key] = std::vector<uint8_t>(bytes, bytes + length);
  return ESP_OK;
}

esp_err_t nvs_erase_key(const nvs_handle_t handle, const char* key) {
  return nvsNamespace(handle).erase(key) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }

esp_err_t nvs_get_stats(const char*, nvs_stats_t* stats) {
  *stats = {};
  stats->available_entries = fake::world().nvs.availableEntries;
  stats->total_entries = 504;
  return ESP_OK;
}

void esp_fill_random(void* buffer, const size_t length) {
  auto* bytes = static_cast<uint8_t*>(buffer);
  for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(i * 37 + 11);
}

// ---- the card
// ---------------------------------------------------------------------------------------------------------

HalStorage& HalStorage::getInstance() {
  static HalStorage instance;
  return instance;
}
bool HalStorage::ready() const { return fake::world().card.ready; }
UsbDriveState HalStorage::usbDriveState() const { return UsbDriveState::Unsupported; }
bool HalStorage::exists(const char* path) {
  const auto& files = fake::world().card.files;
  if (files.count(path)) return true;
  const std::string prefix = std::string(path) + "/";
  return std::any_of(files.begin(), files.end(), [&](const auto& f) { return f.first.rfind(prefix, 0) == 0; });
}
bool HalStorage::mkdir(const char*, bool) { return true; }
bool HalStorage::remove(const char* path) { return fake::world().card.files.erase(path) > 0; }
bool HalStorage::replaceFile(const char* tmpPath, const char* path) {
  auto& files = fake::world().card.files;
  const auto found = files.find(tmpPath);
  if (found == files.end()) return false;
  files[path] = found->second;
  files.erase(tmpPath);
  return true;
}
bool HalStorage::openFileForRead(const char*, const char* path, HalFile& file) {
  if (!fake::world().card.files.count(path)) return false;
  file = HalFile();
  file.path_ = path;
  file.open_ = true;
  return true;
}
bool HalStorage::openFileForRead(const char* module, const std::string& path, HalFile& file) {
  return openFileForRead(module, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char*, const char* path, HalFile& file) {
  fake::world().card.files[path].clear();
  file = HalFile();
  file.path_ = path;
  file.open_ = true;
  file.writable_ = true;
  return true;
}
bool HalStorage::readFileToString(const char*, const std::string& path, const size_t cap, std::string& out) {
  const auto& files = fake::world().card.files;
  const auto found = files.find(path);
  if (found == files.end() || found->second.empty() || found->second.size() > cap) return false;
  out = found->second;
  return true;
}
HalFile HalStorage::open(const char* path, const oflag_t flags) {
  HalFile file;
  if (!fake::world().card.files.count(path)) return file;
  file.path_ = path;
  file.open_ = true;
  file.writable_ = (flags & O_RDWR) != 0;
  return file;
}

size_t HalFile::fileSize() { return fake::world().card.files[path_].size(); }
int HalFile::read(void* buffer, const size_t count) {
  const std::string& data = fake::world().card.files[path_];
  if (position_ + count > data.size()) return -1;
  std::memcpy(buffer, data.data() + position_, count);
  position_ += count;
  return static_cast<int>(count);
}
size_t HalFile::write(const uint8_t* buffer, const size_t count) {
  if (!writable_) return 0;
  std::string& data = fake::world().card.files[path_];
  if (position_ + count > data.size()) data.resize(position_ + count);
  std::memcpy(&data[position_], buffer, count);
  position_ += count;
  return count;
}
bool HalFile::seekSet(const size_t offset) {
  position_ = offset;
  return true;
}
bool HalFile::close() {
  open_ = false;
  return true;
}

// ---- the saved networks
// -----------------------------------------------------------------------------------------------

WifiCredentialStore& WifiCredentialStore::getInstance() {
  static WifiCredentialStore instance;
  return instance;
}
void WifiCredentialStore::loadFromFile() { ++fake::world().store.loads; }
std::optional<WifiCredential> WifiCredentialStore::getCredentialAt(const size_t index) const {
  const auto& store = fake::world().store;
  if (index >= store.networks.size()) {
    ++fake::world().store.outOfRangeReads;
    return std::nullopt;
  }
  return WifiCredential{store.networks[index].ssid, store.networks[index].password};
}
std::optional<std::string> WifiCredentialStore::getSsidAt(const size_t index) const {
  const auto& store = fake::world().store;
  if (index >= store.networks.size()) {
    ++fake::world().store.outOfRangeReads;
    return std::nullopt;
  }
  return store.networks[index].ssid;
}
size_t WifiCredentialStore::getCredentialCount() const { return fake::world().store.networks.size(); }
std::vector<WifiCredentialSummary> WifiCredentialStore::getCredentialSummaries() const {
  std::vector<WifiCredentialSummary> out;
  const auto& store = fake::world().store;
  for (const auto& network : store.networks)
    out.push_back({network.ssid, !network.password.empty(), network.ssid == store.lastConnected});
  return out;
}
void WifiCredentialStore::setLastConnectedSsid(const std::string& ssid) { fake::world().store.lastConnected = ssid; }
std::string WifiCredentialStore::getLastConnectedSsid() const { return fake::world().store.lastConnected; }

// ---- the render lock, the host name, the clock
// ------------------------------------------------------------------------

RenderLock::RenderLock(const Mode mode) {
  isLocked = !(mode == Mode::Try && fake::world().renderLockBusy);
  if (isLocked) ++fake::world().renderLockTaken;
}
RenderLock::~RenderLock() = default;

namespace devicename {
void applyStationHostname() { ++fake::world().hostnameApplied; }
}  // namespace devicename

// ---- the system clock, the RTC chip, Preferences
// -----------------------------------------------------------------------

int glue_gettimeofday(struct timeval* tv, void*) {
  tv->tv_sec = static_cast<time_t>(fake::world().clock.timeEpoch);
  tv->tv_usec = 0;
  return 0;
}
time_t glue_time(time_t* out) {
  const time_t now = static_cast<time_t>(fake::world().clock.timeEpoch);
  if (out) *out = now;
  return now;
}
int glue_settimeofday(const struct timeval* tv, const void*) {
  fake::world().clock.timeEpoch = tv->tv_sec;
  fake::world().clock.trail.push_back("settimeofday");
  return 0;
}

uint64_t esp_rtc_get_time_us() { return fake::world().clock.rtcMicros; }
esp_reset_reason_t esp_reset_reason() { return static_cast<esp_reset_reason_t>(fake::world().clock.resetReason); }
uint32_t esp_random() { return 0x5eed1234; }

HalClock halClock;
bool HalClock::writeUtc(const int64_t epoch) {
  fake::Chip& chip = fake::world().chip;
  if (!chip.writable) return false;
  chip.written.push_back(epoch);
  chip.epoch = epoch;
  chip.setAtRtcMicros = fake::world().clock.rtcMicros;
  chip.readable = true;
  return true;
}
bool HalClock::readUtc(int64_t& epoch) {
  const fake::World& w = fake::world();
  if (!w.chip.readable) return false;
  epoch = w.chip.epoch + static_cast<int64_t>((w.clock.rtcMicros - w.chip.setAtRtcMicros) / 1000000);
  return true;
}

bool Preferences::begin(const char* name, const bool readOnly) {
  auto& data = fake::world().nvs.data;
  if (readOnly && data.find(name) == data.end()) return false;
  data[name];
  space_ = name;
  open_ = true;
  return true;
}
bool Preferences::isKey(const char* key) {
  const auto& space = fake::world().nvs.data[space_];
  return open_ && space.count(key) > 0;
}
size_t Preferences::getBytesLength(const char* key) {
  const auto& space = fake::world().nvs.data[space_];
  const auto found = space.find(key);
  return open_ && found != space.end() ? found->second.size() : 0;
}
size_t Preferences::getBytes(const char* key, void* buffer, const size_t length) {
  const auto& space = fake::world().nvs.data[space_];
  const auto found = space.find(key);
  if (!open_ || found == space.end() || found->second.size() > length) return 0;
  std::memcpy(buffer, found->second.data(), found->second.size());
  return found->second.size();
}
int64_t Preferences::getLong64(const char* key, const int64_t fallback) {
  const auto& space = fake::world().nvs.data[space_];
  const auto found = space.find(key);
  if (!open_ || found == space.end() || found->second.size() != sizeof(int64_t)) return fallback;
  int64_t value = 0;
  std::memcpy(&value, found->second.data(), sizeof(value));
  return value;
}
size_t Preferences::putLong64(const char* key, const int64_t value) {
  if (!open_) return 0;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
  fake::world().nvs.data[space_][key] = std::vector<uint8_t>(bytes, bytes + sizeof(value));
  return sizeof(value);
}
bool Preferences::remove(const char* key) { return open_ && fake::world().nvs.data[space_].erase(key) > 0; }
size_t Preferences::putBytes(const char* key, const void* value, const size_t length) {
  if (!open_) return 0;
  const auto* bytes = static_cast<const uint8_t*>(value);
  fake::world().nvs.data[space_][key] = std::vector<uint8_t>(bytes, bytes + length);
  return length;
}

// ---- esp_sntp, the certificate bundle, the power-save mode
// --------------------------------------------------------------

void esp_sntp_set_sync_status(const sntp_sync_status_t value) {
  fake::world().clock.sntpStatusPending = value == SNTP_SYNC_STATUS_COMPLETED;
}
sntp_sync_status_t sntp_get_sync_status() {
  fake::sntpStep();
  fake::Clock& c = fake::world().clock;
  if (!c.sntpStatusPending) return SNTP_SYNC_STATUS_RESET;
  c.sntpStatusPending = false;  // the read consumes it, as esp_sntp.h documents
  return SNTP_SYNC_STATUS_COMPLETED;
}
void sntp_set_time_sync_notification_cb(void (*callback)(struct timeval*)) { syncCallback = callback; }
bool esp_sntp_enabled() { return fake::world().clock.sntpEnabled; }
void esp_sntp_stop() {
  fake::Clock& c = fake::world().clock;
  ++c.sntpStops;
  c.sntpEnabled = false;
}
const char* esp_sntp_getservername(const u8_t idx) {
  const fake::Clock& c = fake::world().clock;
  return c.slotNamed[idx] ? c.slotName[idx] : nullptr;
}
const ip_addr_t* esp_sntp_getserver(const u8_t idx) {
  static ip_addr_t address[3];
  address[idx].addr = fake::world().clock.slotAddress[idx];
  return &address[idx];
}
void esp_sntp_setserver(const u8_t idx, const ip_addr_t* addr) {
  fake::Clock& c = fake::world().clock;
  c.slotAddress[idx] = addr ? addr->addr : 0;
  c.slotNamed[idx] = false;
}
void esp_sntp_servermode_dhcp(const bool enable) {
  fake::Clock& c = fake::world().clock;
  c.dhcpMode = enable;
  c.dhcpFlagSet.push_back(enable);
  c.trail.push_back(enable ? "dhcpOn" : "dhcpOff");
}
// Arduino's configTzTime(): names the slots it is given (a null leaves the slot's name as it was) and (re)starts the
// client.
void configTzTime(const char*, const char* server1, const char* server2, const char* server3) {
  fake::Clock& c = fake::world().clock;
  const char* given[3] = {server1, server2, server3};
  std::vector<std::string> names;
  for (int i = 0; i < 3; ++i) {
    if (!given[i]) continue;
    std::snprintf(c.slotName[i], sizeof(c.slotName[i]), "%s", given[i]);
    c.slotNamed[i] = true;
    names.push_back(given[i]);
  }
  c.serversSet.push_back(names);
  c.trail.push_back("configTzTime");
  ++c.configTzCalls;
  c.sntpEnabled = true;
  c.sntpFired = false;
  c.sntpStatusPending = false;
  c.sntpStartedAt = static_cast<uint32_t>(fake::world().now);
}
esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
bool esp_crt_bundle_in_use(const mbedtls_x509_crt*) { return false; }
esp_err_t esp_wifi_get_ps(wifi_ps_type_t* type) {
  *type = fake::world().clock.powerSave;
  return ESP_OK;
}
esp_err_t esp_wifi_set_ps(const wifi_ps_type_t type) {
  fake::world().clock.powerSave = type;
  return ESP_OK;
}

// ---- the board
// -----------------------------------------------------------------------------------------------------------

namespace plaque {
Board board() { return boardFor(fake::world().boardFacts); }
}  // namespace plaque
