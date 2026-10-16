#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The reader's own identity on the mailbox (docs/SPEC-DEVICES.md section 4). On its first use the reader makes a random
// UUID v4 and keeps it in NVS, which is the source of truth: it survives a card swap and an app reflash, and only a
// full flash erase makes a new one. It is mirrored to /Lamina/reader.json so the Mac can read it over USB, together
// with what the Mac needs to set the reader up before it has ever been online (schema 2: the model, the station and
// hotspot Wi-Fi addresses, the bands and the capabilities; docs/x4c-support.md). The file is written at every boot
// when it does not match. Every mailbox request then carries three headers: X-Lamina-Reader (the UUID),
// X-Lamina-Firmware and X-Lamina-Battery.
//
// This file holds the pure parts, covered by test/reader_identity: the UUID, the header values, the file and the
// two decisions (what NVS says, whether the file is right). The NVS and card adapters are in ReaderIdentityDevice.cpp.
namespace readerid {

constexpr size_t UUID_BYTES = 16;
constexpr size_t UUID_CHARS = 36;          // 8-4-4-4-12 with hyphens, lowercase hex
constexpr size_t FIRMWARE_MAX_CHARS = 40;  // what the Worker accepts
constexpr size_t SHORT_CHARS = 8;
constexpr char CARD_FOLDER[] = "/Lamina";
constexpr char CARD_PATH[] = "/Lamina/reader.json";
constexpr char CARD_TMP_PATH[] = "/Lamina/reader.json.tmp";
constexpr size_t CARD_MAX_BYTES = 1024;  // what is read back; a schema 2 file is under 512
constexpr int CARD_SCHEMA = 2;
constexpr size_t MAC_BYTES = 6;
constexpr char HEADER_READER[] = "X-Lamina-Reader";
constexpr char HEADER_FIRMWARE[] = "X-Lamina-Firmware";
constexpr char HEADER_BATTERY[] = "X-Lamina-Battery";

// ---- values ----------------------------------------------------------------------------

// Sets the version (4) and variant (10) bits of 16 random bytes and writes the canonical lowercase text.
std::string uuidV4(const uint8_t random[UUID_BYTES]);
// The canonical text of 16 bytes as they are.
std::string uuidText(const uint8_t bytes[UUID_BYTES]);
// Lowercase 8-4-4-4-12 hex, version 4, variant 10xx: what uuidV4() makes.
bool validUuid(std::string_view text);
// The first 8 characters, for a screen with room for little.
std::string shortId(std::string_view uuid);

// The firmware version as the Worker wants it: printable ASCII only, no leading or trailing space, at most 40
// characters. "unknown" when nothing usable is left, so the header is always valid.
std::string cleanFirmware(std::string_view version);
// A battery reading as the Worker wants it: 0 to 100.
int clampBattery(int percent);

// The headers every mailbox request carries, in this order. The reader header is left out when `uuid` is not a valid
// UUID (the reader has no identity yet): a made-up one would change on the next boot.
std::vector<std::pair<std::string, std::string>> headers(std::string_view uuid, std::string_view firmware,
                                                         int batteryPercent);

// A Wi-Fi address as reader.json and Settings › About write it: "AA:BB:CC:DD:EE:FF".
std::string macText(const uint8_t mac[MAC_BYTES]);

// What /Lamina/reader.json says. An empty text field, or an empty list, is left out of the file.
struct CardInfo {
  std::string uuid;
  std::string firmware;
  int schema = CARD_SCHEMA;  // what a parsed file declared; 1 when it declared nothing
  std::string model;         // "x4pro", "x4c"
  std::string modelName;     // "Xteink X4 Classic"
  std::string stationMac;    // the address Wi-Fi registration needs
  std::string hotspotMac;    // the reader's own File Transfer hotspot: never registered
  std::vector<std::string> wifiBands;
  std::vector<std::string> capabilities;
};

// The file: {"reader_uuid":"...","firmware":"...","schema":2,"model":"...","model_name":"...","station_mac":"...",
// "hotspot_mac":"...","wifi_bands":[...],"capabilities":[...]} on one line, and a newline. The firmware is cleaned.
std::string cardJson(const CardInfo& info);
// Reads any reader.json, old or new: a schema 1 file (reader_uuid and firmware alone) parses, a missing field stays
// empty, an unknown one is ignored, and a field of the wrong type counts as missing. False when the text is not a JSON
// object of at most CARD_MAX_BYTES.
bool parseCard(std::string_view text, CardInfo& out);
// True when the text is a reader.json that already says everything `info` says, and nothing more of what this reader
// writes: a file from older firmware (schema 1, or a field missing or different) does not match and is rewritten.
// Unknown fields are ignored.
bool cardMatches(std::string_view text, const CardInfo& info);

// ---- the two edges ---------------------------------------------------------------------

// The reader's non-volatile storage: NVS on the device.
class KeyPort {
 public:
  // Value: exactly 16 bytes are stored. Missing: nothing is stored under the name. Corrupt: something is, of a size
  // that is not 16. Failed: the storage could not be read at all (not the same as empty: nothing is created then).
  enum class Read : uint8_t { Value, Missing, Corrupt, Failed };
  virtual ~KeyPort() = default;
  virtual Read read(uint8_t out[UUID_BYTES]) = 0;
  virtual bool write(const uint8_t in[UUID_BYTES]) = 0;  // stored and committed
};

class RandomPort {
 public:
  virtual ~RandomPort() = default;
  virtual void fill(uint8_t out[UUID_BYTES]) = 0;
};

// The card's /Lamina/reader.json.
class CardPort {
 public:
  virtual ~CardPort() = default;
  virtual bool read(std::string& out) = 0;        // false when missing, empty or unreadable
  virtual bool write(std::string_view text) = 0;  // makes /Lamina, writes the temp file, replaces the file
};

struct Identity {
  enum class Source : uint8_t { Stored, Created, Unavailable };
  std::string uuid;  // empty when Unavailable
  Source source = Source::Unavailable;
};

// What NVS holds becomes the identity. Only a missing (or corrupt) value is replaced, by a new random one that is
// written before it is used; a storage that cannot be read, or written, gives no identity at all, because an
// identity that is not stored would be a different one on the next boot.
Identity resolve(KeyPort& key, RandomPort& random);

enum class Mirror : uint8_t { InSync, Written, Failed };
// Makes the card's file say what NVS (and the board) says. Reads first and writes only when the file is missing or
// different.
Mirror mirror(CardPort& card, const CardInfo& info);

}  // namespace readerid

// The device side (ReaderIdentityDevice.cpp).
namespace ambient {
// The reader's UUID, made and stored on first use, mirrored to the card once per boot on a board with the card's copy
// (plaque::Capabilities::cardIdentity). Empty when NVS is unavailable.
std::string readerUuid();
// Brings /Lamina/reader.json up to date: setup() calls it once the card is mounted, before any network, so the file
// is right at every boot. Never while USB Drive owns the card; a read and a compare when the file is already right.
void syncReaderCard();
// The reader.json this reader writes, as text (for the USB setup link's INFO answer). Empty without an identity.
std::string readerCardText();
// Appends X-Lamina-Reader, X-Lamina-Firmware and X-Lamina-Battery (the gauge read now) to a mailbox request's
// headers. The same pair type as HttpDownloader::Header.
void appendIdentityHeaders(std::vector<std::pair<std::string, std::string>>& headers);
}  // namespace ambient
