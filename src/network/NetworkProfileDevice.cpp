#include "NetworkProfileDevice.h"

#include <HalStorage.h>
#include <Logging.h>
#include <PlaqueBoard.h>
#include <TrustedTime.h>
#include <esp_random.h>
#include <nvs.h>

#include <algorithm>
#include <mutex>

#include "NetworkProfileStore.h"
#include "network/MessageTls.h"

// The reader's side of the network profile (see NetworkProfileDevice.h): NVS through nvs_*, the card through
// HalStorage. Only sizes and result codes are logged, never a value.
namespace netprofile {
namespace {

constexpr char LAMINA_FOLDER[] = "/Lamina";
constexpr char SUMMARY_TMP_PATH[] = "/Lamina/network-profile.json.tmp";
constexpr char RESULT_TMP_PATH[] = "/Lamina/network-import.json.tmp";
constexpr size_t SUMMARY_MAX_BYTES = 4096;
constexpr size_t WIPE_CHUNK = 256;

class DeviceNvs final : public NvsPort {
  nvs_handle_t handle_ = 0;
  esp_err_t opened_ = ESP_FAIL;

  static NvsResult map(const esp_err_t err) {
    switch (err) {
      case ESP_OK:
        return NvsResult::Ok;
      case ESP_ERR_NVS_NOT_FOUND:
        return NvsResult::NotFound;
      case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return NvsResult::NoSpace;
      default:
        return NvsResult::Failed;
    }
  }

 public:
  // Reading never creates the namespace: with nothing ever installed, the open itself says "not found".
  explicit DeviceNvs(const bool write) {
    opened_ = nvs_open(NVS_NAMESPACE, write ? NVS_READWRITE : NVS_READONLY, &handle_);
  }
  ~DeviceNvs() override {
    if (opened_ == ESP_OK) nvs_close(handle_);
  }
  DeviceNvs(const DeviceNvs&) = delete;
  DeviceNvs& operator=(const DeviceNvs&) = delete;

  NvsResult opening() const { return map(opened_); }

  NvsResult getU8(const char* key, uint8_t& value) override {
    if (opened_ != ESP_OK) return map(opened_);
    return map(nvs_get_u8(handle_, key, &value));
  }
  NvsResult setU8(const char* key, const uint8_t value) override {
    if (opened_ != ESP_OK) return NvsResult::Failed;
    return map(nvs_set_u8(handle_, key, value));
  }
  NvsResult blobSize(const char* key, size_t& size) override {
    if (opened_ != ESP_OK) return map(opened_);
    size = 0;
    return map(nvs_get_blob(handle_, key, nullptr, &size));
  }
  NvsResult getBlob(const char* key, uint8_t* out, const size_t size) override {
    if (opened_ != ESP_OK) return map(opened_);
    size_t length = size;
    const NvsResult result = map(nvs_get_blob(handle_, key, out, &length));
    return result == NvsResult::Ok && length != size ? NvsResult::Failed : result;
  }
  NvsResult setBlob(const char* key, const uint8_t* data, const size_t size) override {
    if (opened_ != ESP_OK) return NvsResult::Failed;
    return map(nvs_set_blob(handle_, key, data, size));
  }
  NvsResult erase(const char* key) override {
    if (opened_ != ESP_OK) return map(opened_);
    return map(nvs_erase_key(handle_, key));
  }
  NvsResult commit() override {
    if (opened_ != ESP_OK) return NvsResult::Failed;
    return map(nvs_commit(handle_));
  }
  bool availableEntries(size_t& entries) override {
    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) != ESP_OK) return false;
    entries = stats.available_entries;
    return true;
  }
};

bool cardUsable() { return Storage.ready() && Storage.usbDriveState() == UsbDriveState::Unsupported; }

// What this board offers of the network profile (lib/PlaqueBoard). A board without it has no profile at all: none is
// installed, shown, joined or described on the card, and nothing can be put there.
plaque::Capabilities boardCaps() { return plaque::board().caps; }

// Validation accepts the kinds of network the board supports and no others.
void applyBoard(ValidateOptions& options) {
  const plaque::Capabilities caps = boardCaps();
  options.allowOpen = plaque::supportsWifi(caps, plaque::WifiKind::Open);
  options.allowPsk = plaque::supportsWifi(caps, plaque::WifiKind::Psk);
  options.allowEapTls = plaque::supportsWifi(caps, plaque::WifiKind::EapTls);
}

bool writeCardFile(const char* path, const char* tmpPath, const std::string_view text) {
  if (!Storage.exists(LAMINA_FOLDER) && !Storage.mkdir(LAMINA_FOLDER)) return false;
  HalFile file;
  if (!Storage.openFileForWrite("NETP", tmpPath, file)) return false;
  const size_t written = file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  file.flush();
  if (!file.close() || written != text.size()) {
    Storage.remove(tmpPath);
    return false;
  }
  if (Storage.replaceFile(tmpPath, path)) return true;
  Storage.remove(tmpPath);
  return false;
}

class DeviceCard final : public CardFiles {
 public:
  bool available() override { return cardUsable(); }

  int64_t inboxSize() override {
    if (!Storage.exists(CARD_INBOX_PATH)) return -1;
    HalFile file;
    if (!Storage.openFileForRead("NETP", CARD_INBOX_PATH, file)) return -1;
    return static_cast<int64_t>(file.fileSize());
  }

  bool readInbox(uint8_t* out, const size_t size) override {
    HalFile file;
    if (!Storage.openFileForRead("NETP", CARD_INBOX_PATH, file)) return false;
    return file.read(out, size) == static_cast<int>(size);
  }

  bool wipeInbox() override {
    bool zeroed = false;
    {
      // Opened for writing without truncation, so the zeros land on the clusters that held the bundle.
      HalFile file = Storage.open(CARD_INBOX_PATH, O_RDWR);
      if (file.isOpen()) {
        const size_t size = file.fileSize();
        uint8_t zeros[WIPE_CHUNK] = {};
        zeroed = file.seekSet(0);
        for (size_t done = 0; zeroed && done < size;) {
          const size_t chunk = std::min(size - done, WIPE_CHUNK);
          zeroed = file.write(zeros, chunk) == chunk;
          done += chunk;
        }
        file.flush();
        zeroed = file.close() && zeroed;
      }
    }
    const bool removed = Storage.remove(CARD_INBOX_PATH);
    if (!zeroed || !removed) LOG_ERR("NETP", "Card inbox: zeroed=%d removed=%d", zeroed, removed);
    return zeroed && removed;
  }

  bool writeResult(const std::string_view json) override {
    return writeCardFile(CARD_RESULT_PATH, RESULT_TMP_PATH, json);
  }
};

int deviceRandom(void*, unsigned char* out, const size_t size) {
  esp_fill_random(out, size);
  return 0;
}

// The summary is read from NVS once and kept: the screens, the clock and the PROFILE reply ask for it often.
std::mutex cacheMutex;
Summary cachedSummary;
bool cacheLoaded = false;

void storeSummary(Summary&& fresh) {
  std::lock_guard<std::mutex> lock(cacheMutex);
  cachedSummary = std::move(fresh);
  cacheLoaded = true;
}

void reloadCache() {
  Summary fresh;
  if (!plaque::offersNetworkProfile(boardCaps())) {
    storeSummary(std::move(fresh));  // not a board that has network profiles: none is read, whatever NVS holds
    return;
  }
  DeviceNvs nvs(false);
  if (nvs.opening() == NvsResult::Ok) {
    Store store(nvs);
    switch (store.readSummary(fresh)) {
      case Store::Read::Ok:
      case Store::Read::None:
        break;
      case Store::Read::Corrupt:
        LOG_ERR("NETP", "The stored network profile does not check out: ignored");
        break;
      case Store::Read::Failed:
        LOG_ERR("NETP", "Could not read the network profile from NVS");
        break;
    }
  } else if (nvs.opening() != NvsResult::NotFound) {
    LOG_ERR("NETP", "Could not open NVS for the network profile");
  }
  storeSummary(std::move(fresh));
}

void ensureCache() {
  {
    std::lock_guard<std::mutex> lock(cacheMutex);
    if (cacheLoaded) return;
  }
  reloadCache();
}

void logSpace(const char* when) {
  nvs_stats_t stats = {};
  if (nvs_get_stats(nullptr, &stats) == ESP_OK)
    LOG_INF("NETP", "NVS %s: %u used, %u available of %u entries", when, static_cast<unsigned>(stats.used_entries),
            static_cast<unsigned>(stats.available_entries), static_cast<unsigned>(stats.total_entries));
}

}  // namespace

Summary summary() {
  ensureCache();
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cachedSummary;
}

void reload() { reloadCache(); }

bool installed() {
  ensureCache();
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cachedSummary.installed;
}

std::vector<std::string> activeTimeServers() {
  ensureCache();
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cachedSummary.installed ? cachedSummary.timeServers : std::vector<std::string>();
}

bool activeUseDhcpTime() {
  ensureCache();
  std::lock_guard<std::mutex> lock(cacheMutex);
  return cachedSummary.installed && cachedSummary.useDhcpTime;
}

void applyTimeSettings() {
  const std::vector<std::string> servers = activeTimeServers();
  std::vector<const char*> names;
  names.reserve(servers.size());
  for (const std::string& server : servers) names.push_back(server.c_str());
  if (!trustedtime::setServers(names.empty() ? nullptr : names.data(), names.size())) {
    LOG_ERR("NETP", "The profile's time servers were refused: using the defaults");
    trustedtime::setServers(nullptr, 0);
  }
  trustedtime::setUseDhcpServers(activeUseDhcpTime());
}

void applyDhcpTimeSetting() { trustedtime::setUseDhcpServers(activeUseDhcpTime()); }

bool loadForJoin(Profile& out) {
  if (!plaque::offersNetworkProfile(boardCaps())) return false;
  DeviceNvs nvs(false);
  if (nvs.opening() != NvsResult::Ok) return false;
  Store store(nvs);
  return store.read(out) == Store::Read::Ok;
}

int64_t trustedTime() { return message_tls::certificateClockTrusted() ? trustedtime::trustedNow() : 0; }

Error validate(const std::string_view text, const Source source, Profile& out) {
  ValidateOptions options;
  options.trustedNow = trustedTime();
  options.random = deviceRandom;
  options.source = source;
  applyBoard(options);
  const Error error = source == Source::Card ? parseCardBundle(text, options, out) : parseBundle(text, options, out);
  LOG_INF("NETP", "Bundle of %u bytes from %s: %s (dates %s)", static_cast<unsigned>(text.size()), sourceName(source),
          errorCode(error), options.trustedNow ? "judged" : "not judged");
  return error;
}

Error install(const Profile& profile) {
  if (!plaque::offersNetworkProfile(boardCaps())) return Error::AuthUnsupported;
  logSpace("before install");
  LOG_INF("NETP", "Installing a %s profile: %u bytes, %u NVS entries", authName(profile.info.auth),
          static_cast<unsigned>(Store::storedBytes(profile)), static_cast<unsigned>(Store::entriesNeeded(profile)));
  Error error = Error::None;
  {
    DeviceNvs nvs(true);
    Store store(nvs);
    error = nvs.opening() == NvsResult::Ok ? store.install(profile) : Error::StorageFailed;
  }
  LOG_INF("NETP", "Install: %s", errorCode(error));
  logSpace("after install");
  reloadCache();
  refreshSummaryFile();
  return error;
}

bool remove() {
  bool removed = false;
  {
    DeviceNvs nvs(true);
    Store store(nvs);
    removed = nvs.opening() == NvsResult::Ok && store.remove();
  }
  LOG_INF("NETP", "Remove: %s", removed ? "done" : "failed");
  reloadCache();
  refreshSummaryFile();
  return removed;
}

std::string summaryText() { return summaryJson(summary()); }

void refreshSummaryFile() {
  if (!plaque::writesProfileSummary(boardCaps()) || !cardUsable()) return;
  const std::string text = summaryText();
  std::string onCard;
  if (Storage.exists(CARD_SUMMARY_PATH) &&
      Storage.readFileToString("NETP", CARD_SUMMARY_PATH, SUMMARY_MAX_BYTES, onCard) && onCard == text)
    return;
  if (writeCardFile(CARD_SUMMARY_PATH, SUMMARY_TMP_PATH, text)) {
    LOG_INF("NETP", "%s written", CARD_SUMMARY_PATH);
  } else {
    LOG_ERR("NETP", "Could not write %s", CARD_SUMMARY_PATH);
  }
}

CardRead beginCardImport() {
  if (!plaque::offersCardImport(boardCaps())) {
    CardRead none;
    none.outcome = CardOutcome::NoCard;  // not this board's: the card is not looked at
    return none;
  }
  DeviceCard card;
  ValidateOptions options;
  options.trustedNow = trustedTime();
  options.random = deviceRandom;
  options.source = Source::Card;
  applyBoard(options);
  CardRead read = readCard(card, options, trustedTime());
  LOG_INF("NETP", "Import from card: outcome %d, %s, pending=%d, wiped=%d, result file=%d",
          static_cast<int>(read.outcome), errorCode(read.error), read.pending, read.wiped, read.resultWritten);
  return read;
}

CardResult finishCardImport(CardRead& read, const bool install) {
  DeviceCard card;
  CardResult result;
  {
    DeviceNvs nvs(true);
    Store store(nvs);
    result = finishCard(card, store, read, install, trustedTime());
  }
  LOG_INF("NETP", "Import from card finished: outcome %d, %s, result file=%d", static_cast<int>(result.outcome),
          errorCode(result.error), result.resultWritten);
  if (result.outcome == CardOutcome::Installed) {
    reloadCache();
    refreshSummaryFile();
  }
  return result;
}

}  // namespace netprofile
