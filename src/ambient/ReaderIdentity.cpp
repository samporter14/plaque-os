#include "ReaderIdentity.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>

namespace readerid {
namespace {
constexpr char FALLBACK_FIRMWARE[] = "unknown";

bool lowerHex(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

std::string format(const uint8_t bytes[UUID_BYTES]) {
  char text[UUID_CHARS + 1];
  snprintf(text, sizeof(text), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0],
           bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
           bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
  return text;
}

void setV4Bits(uint8_t bytes[UUID_BYTES]) {
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0F) | 0x40);  // version 4
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3F) | 0x80);  // variant 10xx
}

// The 16 bytes behind a canonical UUID text; false when the text is not one (version and variant included).
bool bytesOf(std::string_view text, uint8_t out[UUID_BYTES]) {
  if (text.size() != UUID_CHARS) return false;
  size_t nibbles = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
    if (hyphen) {
      if (text[i] != '-') return false;
      continue;
    }
    if (!lowerHex(text[i])) return false;
    const uint8_t value = static_cast<uint8_t>(text[i] <= '9' ? text[i] - '0' : text[i] - 'a' + 10);
    if (nibbles % 2 == 0) {
      out[nibbles / 2] = static_cast<uint8_t>(value << 4);
    } else {
      out[nibbles / 2] = static_cast<uint8_t>(out[nibbles / 2] | value);
    }
    ++nibbles;
  }
  return (out[6] >> 4) == 4 && (out[8] >> 6) == 2;
}
}  // namespace

std::string uuidV4(const uint8_t random[UUID_BYTES]) {
  uint8_t bytes[UUID_BYTES];
  std::copy(random, random + UUID_BYTES, bytes);
  setV4Bits(bytes);
  return format(bytes);
}

std::string uuidText(const uint8_t bytes[UUID_BYTES]) { return format(bytes); }

bool validUuid(const std::string_view text) {
  uint8_t bytes[UUID_BYTES];
  return bytesOf(text, bytes);
}

std::string shortId(const std::string_view uuid) { return std::string(uuid.substr(0, SHORT_CHARS)); }

std::string cleanFirmware(const std::string_view version) {
  std::string out;
  for (const char c : version) {
    const auto byte = static_cast<unsigned char>(c);  // char is unsigned on the reader and signed on a host
    if (byte >= 0x20 && byte <= 0x7E) out.push_back(c);
  }
  const size_t first = out.find_first_not_of(' ');
  if (first == std::string::npos) return FALLBACK_FIRMWARE;
  out.erase(0, first);
  if (out.size() > FIRMWARE_MAX_CHARS) out.resize(FIRMWARE_MAX_CHARS);
  out.erase(out.find_last_not_of(' ') + 1);  // a cut can leave a space at the end
  return out.empty() ? std::string(FALLBACK_FIRMWARE) : out;
}

int clampBattery(const int percent) { return std::clamp(percent, 0, 100); }

std::vector<std::pair<std::string, std::string>> headers(const std::string_view uuid, const std::string_view firmware,
                                                         const int batteryPercent) {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(3);
  if (validUuid(uuid)) out.emplace_back(HEADER_READER, std::string(uuid));
  out.emplace_back(HEADER_FIRMWARE, cleanFirmware(firmware));
  out.emplace_back(HEADER_BATTERY, std::to_string(clampBattery(batteryPercent)));
  return out;
}

std::string macText(const uint8_t mac[MAC_BYTES]) {
  char text[MAC_BYTES * 3];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return text;
}

namespace {
void putText(JsonDocument& doc, const char* key, const std::string& value) {
  if (!value.empty()) doc[key] = value;
}

void putList(JsonDocument& doc, const char* key, const std::vector<std::string>& values) {
  if (values.empty()) return;
  JsonArray list = doc[key].to<JsonArray>();
  for (const std::string& value : values) list.add(value);
}

std::string textOf(JsonVariantConst value) { return value.is<const char*>() ? value.as<const char*>() : ""; }

// The strings of a list; anything that is not a string is skipped.
std::vector<std::string> listOf(JsonVariantConst value) {
  std::vector<std::string> out;
  if (!value.is<JsonArrayConst>()) return out;
  const JsonArrayConst list = value.as<JsonArrayConst>();
  out.reserve(list.size());
  for (const JsonVariantConst item : list)
    if (item.is<const char*>()) out.emplace_back(item.as<const char*>());
  return out;
}
}  // namespace

std::string cardJson(const CardInfo& info) {
  JsonDocument doc;
  doc["reader_uuid"] = info.uuid;
  doc["firmware"] = cleanFirmware(info.firmware);
  doc["schema"] = CARD_SCHEMA;
  putText(doc, "model", info.model);
  putText(doc, "model_name", info.modelName);
  putText(doc, "station_mac", info.stationMac);
  putText(doc, "hotspot_mac", info.hotspotMac);
  putList(doc, "wifi_bands", info.wifiBands);
  putList(doc, "capabilities", info.capabilities);
  std::string text;
  serializeJson(doc, text);
  text.push_back('\n');
  return text;
}

bool parseCard(const std::string_view text, CardInfo& out) {
  out = CardInfo();
  if (text.empty() || text.size() > CARD_MAX_BYTES) return false;
  JsonDocument doc;
  if (deserializeJson(doc, text.data(), text.size())) return false;
  if (!doc.is<JsonObject>()) return false;
  const JsonObjectConst file = doc.as<JsonObjectConst>();
  out.uuid = textOf(file["reader_uuid"]);
  out.firmware = textOf(file["firmware"]);
  out.schema = file["schema"].is<int>() ? file["schema"].as<int>() : 1;
  out.model = textOf(file["model"]);
  out.modelName = textOf(file["model_name"]);
  out.stationMac = textOf(file["station_mac"]);
  out.hotspotMac = textOf(file["hotspot_mac"]);
  out.wifiBands = listOf(file["wifi_bands"]);
  out.capabilities = listOf(file["capabilities"]);
  return true;
}

bool cardMatches(const std::string_view text, const CardInfo& info) {
  CardInfo stored;
  if (!parseCard(text, stored)) return false;
  return stored.schema == CARD_SCHEMA && !stored.uuid.empty() && stored.uuid == info.uuid &&
         stored.firmware == cleanFirmware(info.firmware) && stored.model == info.model &&
         stored.modelName == info.modelName && stored.stationMac == info.stationMac &&
         stored.hotspotMac == info.hotspotMac && stored.wifiBands == info.wifiBands &&
         stored.capabilities == info.capabilities;
}

Identity resolve(KeyPort& key, RandomPort& random) {
  uint8_t bytes[UUID_BYTES] = {};
  switch (key.read(bytes)) {
    case KeyPort::Read::Value: {
      const std::string text = uuidText(bytes);
      if (validUuid(text)) return {text, Identity::Source::Stored};
      break;  // not something this code stored: replaced below
    }
    case KeyPort::Read::Missing:
    case KeyPort::Read::Corrupt:
      break;
    case KeyPort::Read::Failed:
      return {};
  }
  random.fill(bytes);
  setV4Bits(bytes);
  if (!key.write(bytes)) return {};
  return {uuidText(bytes), Identity::Source::Created};
}

Mirror mirror(CardPort& card, const CardInfo& info) {
  std::string text;
  if (card.read(text) && cardMatches(text, info)) return Mirror::InSync;
  return card.write(cardJson(info)) ? Mirror::Written : Mirror::Failed;
}

}  // namespace readerid
