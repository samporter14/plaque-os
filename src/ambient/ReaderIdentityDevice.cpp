#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PlaqueBoard.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>

#include <mutex>

#include "ReaderIdentity.h"

// The reader's side of the identity (see ReaderIdentity.h): NVS for the UUID, the SD card for the mirror. Each
// adapter is a thin translation; the decisions are in the pure code that test/reader_identity covers.
namespace {
// NVS namespace names are limited to 15 characters. Its own namespace, as deviceSecret() has "devid".
constexpr char NVS_NAMESPACE[] = "lamina";
constexpr char NVS_KEY[] = "reader";

class NvsKey final : public readerid::KeyPort {
 public:
  Read read(uint8_t out[readerid::UUID_BYTES]) override {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return Read::Failed;
    size_t size = readerid::UUID_BYTES;
    const esp_err_t err = nvs_get_blob(handle, NVS_KEY, out, &size);
    nvs_close(handle);
    if (err == ESP_OK) return size == readerid::UUID_BYTES ? Read::Value : Read::Corrupt;
    if (err == ESP_ERR_NVS_NOT_FOUND) return Read::Missing;
    if (err == ESP_ERR_NVS_INVALID_LENGTH) return Read::Corrupt;  // a value longer than 16 bytes
    return Read::Failed;
  }

  bool write(const uint8_t in[readerid::UUID_BYTES]) override {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool ok = nvs_set_blob(handle, NVS_KEY, in, readerid::UUID_BYTES) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    return ok;
  }
};

// The hardware generator. The identity is made on the first boot, before Wi-Fi is up, and the ESP32's generator only
// has its full entropy while the radio runs; so the chip's own Wi-Fi MAC address (unique to the chip) and the
// microsecond timer are folded in. XOR with fixed bytes cannot make a uniform draw less uniform, and it keeps two
// readers that first boot at the same instant from drawing the same UUID if the generator is weak.
class HardwareRandom final : public readerid::RandomPort {
 public:
  void fill(uint8_t out[readerid::UUID_BYTES]) override {
    esp_fill_random(out, readerid::UUID_BYTES);
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
      for (size_t i = 0; i < readerid::UUID_BYTES; ++i) out[i] ^= mac[i % sizeof(mac)];
    const uint64_t now = static_cast<uint64_t>(esp_timer_get_time());
    for (size_t i = 0; i < sizeof(now); ++i) out[i * 2] ^= static_cast<uint8_t>(now >> (8 * i));
  }
};

class CardFile final : public readerid::CardPort {
 public:
  bool read(std::string& out) override {
    return Storage.exists(readerid::CARD_PATH) &&
           Storage.readFileToString("IDENT", readerid::CARD_PATH, readerid::CARD_MAX_BYTES, out);
  }

  bool write(std::string_view text) override {
    if (!Storage.exists(readerid::CARD_FOLDER) && !Storage.mkdir(readerid::CARD_FOLDER)) return false;
    HalFile file;
    if (!Storage.openFileForWrite("IDENT", readerid::CARD_TMP_PATH, file)) return false;
    const size_t written = file.write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    file.flush();
    if (!file.close() || written != text.size()) {
      Storage.remove(readerid::CARD_TMP_PATH);
      return false;
    }
    if (Storage.replaceFile(readerid::CARD_TMP_PATH, readerid::CARD_PATH)) return true;
    Storage.remove(readerid::CARD_TMP_PATH);
    return false;
  }
};

// What the file says besides the UUID: this firmware, this board, its two Wi-Fi addresses (read from eFuse, so known
// before Wi-Fi has ever started) and what Plaque offers on it. An address the chip will not give is left out.
readerid::CardInfo cardInfo(const std::string& uuid) {
  readerid::CardInfo info;
  info.uuid = uuid;
  info.firmware = CROSSPOINT_VERSION;
  const plaque::Board board = plaque::board();
  info.model = plaque::modelId(board.model);
  info.modelName = plaque::modelName(board.model);
  uint8_t mac[readerid::MAC_BYTES] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) info.stationMac = readerid::macText(mac);
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) info.hotspotMac = readerid::macText(mac);
  info.wifiBands.emplace_back(plaque::WIFI_BAND_24GHZ);
  const plaque::Tokens tokens = plaque::capabilityTokens(board.caps);
  info.capabilities.reserve(tokens.count);
  for (size_t i = 0; i < tokens.count; ++i) info.capabilities.emplace_back(tokens.items[i]);
  return info;
}

// The card is the reader's to write: mounted, and not handed to a USB host.
bool cardWritable() { return Storage.ready() && Storage.usbDriveState() == UsbDriveState::Unsupported; }
}  // namespace

namespace ambient {
std::string readerUuid() {
  static std::string uuid;
  static bool mirrored = false;
  static std::mutex lock;  // the web server task, the render task and the loop can all ask first
  std::lock_guard<std::mutex> guard(lock);

  // A failure is not cached: the next call retries (as deviceSecret() does).
  if (uuid.empty()) {
    NvsKey key;
    HardwareRandom random;
    const readerid::Identity identity = readerid::resolve(key, random);
    uuid = identity.uuid;
    if (identity.source == readerid::Identity::Source::Created)
      LOG_INF("IDENT", "New reader identity %s", readerid::shortId(uuid).c_str());
    else if (identity.source == readerid::Identity::Source::Unavailable)
      LOG_ERR("IDENT", "Reader identity unavailable: NVS could not be read or written");
  }
  // The card's copy is for the Mac; it is checked once per boot (setup() asks first, see syncReaderCard()), and only on
  // a board that has it. A card that is not the reader's to write right now is tried again on the next call.
  if (!uuid.empty() && !mirrored && plaque::board().caps.cardIdentity && cardWritable()) {
    mirrored = true;
    CardFile card;
    switch (readerid::mirror(card, cardInfo(uuid))) {
      case readerid::Mirror::InSync:
        break;
      case readerid::Mirror::Written:
        LOG_INF("IDENT", "%s written (%s)", readerid::CARD_PATH, readerid::shortId(uuid).c_str());
        break;
      case readerid::Mirror::Failed:
        LOG_ERR("IDENT", "Could not write %s", readerid::CARD_PATH);
        break;
    }
  }
  return uuid;
}

void syncReaderCard() {
  // The identity is made either way; the file waits for a card the reader may write (readerUuid() checks again).
  if (!cardWritable()) LOG_INF("IDENT", "%s not checked: the card is not the reader's to write", readerid::CARD_PATH);
  readerUuid();
}

std::string readerCardText() {
  const std::string uuid = readerUuid();
  return uuid.empty() ? std::string() : readerid::cardJson(cardInfo(uuid));
}

void appendIdentityHeaders(std::vector<std::pair<std::string, std::string>>& headers) {
  for (auto& header : readerid::headers(readerUuid(), CROSSPOINT_VERSION, powerManager.getBatteryPercentage()))
    headers.push_back(std::move(header));
}
}  // namespace ambient
