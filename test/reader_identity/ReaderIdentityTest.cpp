#include <ArduinoJson.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "PlaqueBoard.h"
#include "ReaderIdentity.h"

// Host tests for the reader's identity (src/ambient/ReaderIdentity.*): the UUID, the three header values, the
// /Lamina/reader.json mirror (schema 2: the model, the Wi-Fi addresses, the bands and the capabilities), and what
// happens to the identity across a card swap, an app reflash and a full erase.

using namespace readerid;

namespace {
int checks = 0;

void check(const bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

void check(const bool condition, const std::string& name) { check(condition, name.c_str()); }

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* name) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << name << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
    std::exit(1);
  }
  ++checks;
}

// The reader's flash: survives a card swap and an app reflash, not an erase.
struct FakeNvs : KeyPort {
  bool stored = false;
  uint8_t value[UUID_BYTES] = {};
  Read forced = Read::Value;  // Value: behave; anything else: answer that
  bool useForced = false;
  size_t storedSize = UUID_BYTES;
  bool failWrite = false;
  int reads = 0;
  int writes = 0;

  Read read(uint8_t out[UUID_BYTES]) override {
    ++reads;
    if (useForced) return forced;
    if (!stored) return Read::Missing;
    if (storedSize != UUID_BYTES) return Read::Corrupt;
    std::memcpy(out, value, UUID_BYTES);
    return Read::Value;
  }
  bool write(const uint8_t in[UUID_BYTES]) override {
    ++writes;
    if (failWrite) return false;
    std::memcpy(value, in, UUID_BYTES);
    stored = true;
    storedSize = UUID_BYTES;
    return true;
  }
  void erase() { stored = false; }
};

// What the next random draw is.
struct FakeRandom : RandomPort {
  uint8_t next = 0x10;
  int draws = 0;
  void fill(uint8_t out[UUID_BYTES]) override {
    ++draws;
    for (size_t i = 0; i < UUID_BYTES; ++i) out[i] = static_cast<uint8_t>(next + i * 7);
    next = static_cast<uint8_t>(next + 0x33);
  }
};

// The card: /Lamina/reader.json, replaced as a whole.
struct FakeCard : CardPort {
  bool present = false;
  std::string text;
  bool failWrite = false;
  bool failRead = false;
  int writes = 0;
  bool read(std::string& out) override {
    if (!present || text.empty() || failRead) return false;
    out = text;
    return true;
  }
  bool write(std::string_view data) override {
    ++writes;
    if (failWrite) return false;
    present = true;
    text = std::string(data);
    return true;
  }
};

// What the device side (ReaderIdentityDevice.cpp cardInfo()) puts in the file for a board: its model, the two
// addresses, the band, and the capability tokens from lib/PlaqueBoard.
CardInfo info(const std::string& uuid, const std::string& firmware,
              const plaque::Model model = plaque::Model::X4Classic, const std::string& station = "AA:BB:CC:DD:EE:FF",
              const std::string& hotspot = "AA:BB:CC:DD:EE:F0") {
  CardInfo i;
  i.uuid = uuid;
  i.firmware = firmware;
  i.model = plaque::modelId(model);
  i.modelName = plaque::modelName(model);
  i.stationMac = station;
  i.hotspotMac = hotspot;
  i.wifiBands = {plaque::WIFI_BAND_24GHZ};
  const plaque::Tokens tokens = plaque::capabilityTokens(plaque::capabilitiesFor(model));
  i.capabilities.assign(tokens.items, tokens.items + tokens.count);
  return i;
}

void testUuidV4() {
  uint8_t zeros[UUID_BYTES] = {};
  checkEq(uuidV4(zeros), std::string("00000000-0000-4000-8000-000000000000"), "zeros get only the version and variant");
  uint8_t ones[UUID_BYTES];
  std::memset(ones, 0xFF, sizeof(ones));
  checkEq(uuidV4(ones), std::string("ffffffff-ffff-4fff-bfff-ffffffffffff"), "ones keep their other bits");
  uint8_t mixed[UUID_BYTES] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
                               0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0};
  checkEq(uuidV4(mixed), std::string("12345678-9abc-4ef0-9234-56789abcdef0"),
          "mixed bytes, lowercase, hyphens at 8, 13, 18, 23");
  check(mixed[6] == 0xde && mixed[8] == 0x12, "the input is left alone");
  checkEq(uuidV4(mixed).size(), UUID_CHARS, "36 characters");
  checkEq(uuidText(mixed), std::string("12345678-9abc-def0-1234-56789abcdef0"),
          "uuidText() formats the bytes as they are");

  for (int i = 0; i < 256; ++i) {
    uint8_t random[UUID_BYTES];
    for (size_t k = 0; k < UUID_BYTES; ++k) random[k] = static_cast<uint8_t>(i * 31 + k * 17);
    const std::string text = uuidV4(random);
    check(validUuid(text), "every generated UUID is a valid v4");
    check(text[14] == '4', "version nibble");
    check(std::string("89ab").find(text[19]) != std::string::npos, "variant nibble");
  }
}

void testValidUuid() {
  const std::string good = "12345678-9abc-4ef0-9234-56789abcdef0";
  check(validUuid(good), "a v4 UUID");
  check(!validUuid(""), "empty");
  check(!validUuid(good.substr(0, 35)), "too short");
  check(!validUuid(good + "0"), "too long");
  std::string t = good;
  t[8] = '_';
  check(!validUuid(t), "wrong separator");
  t = good;
  t[3] = 'A';
  check(!validUuid(t), "uppercase");
  t = good;
  t[3] = 'g';
  check(!validUuid(t), "not hex");
  t = good;
  t[14] = '1';
  check(!validUuid(t), "version 1");
  t = good;
  t[19] = '7';
  check(!validUuid(t), "variant 0111");
  t = good;
  t[19] = 'c';
  check(!validUuid(t), "variant 11xx");
  t = good;
  t[19] = '8';
  check(validUuid(t), "variant 8");
  t = good;
  t.replace(0, 8, "{12345-6");
  check(!validUuid(t), "braces");
  check(!validUuid("12345678-9abc-4ef0-9234-56789abcdef\n"), "trailing newline");
  checkEq(shortId(good), std::string("12345678"), "the first 8 characters");
  checkEq(shortId("abc"), std::string("abc"), "a short text stays whole");
  checkEq(shortId(""), std::string(""), "empty stays empty");
}

void testFirmwareText() {
  checkEq(cleanFirmware("1.6.5-x4pro"), std::string("1.6.5-x4pro"), "the real version is untouched");
  checkEq(cleanFirmware("  1.6.5  "), std::string("1.6.5"), "spaces at the ends go");
  checkEq(cleanFirmware("1.6.5 dev"), std::string("1.6.5 dev"), "a space inside stays");
  checkEq(cleanFirmware("1.6\r\nX-Evil: 1"), std::string("1.6X-Evil: 1"), "line breaks never reach a header");
  checkEq(cleanFirmware(std::string("1.\0"
                                    "2",
                                    4)),
          std::string("1.2"), "a NUL goes");
  checkEq(cleanFirmware("v\xC3\xA9"
                        "1"),
          std::string("v1"), "non-ASCII goes");
  checkEq(cleanFirmware("\t1\t"), std::string("1"), "tabs go");
  checkEq(cleanFirmware(""), std::string("unknown"), "empty becomes unknown");
  checkEq(cleanFirmware("   "), std::string("unknown"), "blank becomes unknown");
  checkEq(cleanFirmware("\x01\x02\xFF"), std::string("unknown"), "nothing printable becomes unknown");
  const std::string forty(40, 'a');
  checkEq(cleanFirmware(forty), forty, "40 characters fit");
  checkEq(cleanFirmware(forty + "b"), forty, "41 are cut to 40");
  checkEq(cleanFirmware(std::string(39, 'a') + " tail"), std::string(39, 'a'), "a cut that leaves a space drops it");
  checkEq(cleanFirmware(std::string(60, ' ') + "x").size(), size_t{1}, "leading spaces do not use up the 40");
  checkEq(cleanFirmware("a\"b\\c"), std::string("a\"b\\c"), "quotes and backslashes are printable");

  checkEq(clampBattery(-5), 0, "below zero");
  checkEq(clampBattery(0), 0, "zero");
  checkEq(clampBattery(57), 57, "in range");
  checkEq(clampBattery(100), 100, "full");
  checkEq(clampBattery(101), 100, "above");
  checkEq(clampBattery(65535), 100, "a raw reading");
}

void testHeaders() {
  const std::string uuid = "12345678-9abc-4ef0-9234-56789abcdef0";
  const auto h = headers(uuid, "1.6.5-x4pro", 83);
  checkEq(h.size(), size_t{3}, "three headers");
  checkEq(h[0].first, std::string("X-Lamina-Reader"), "reader name");
  checkEq(h[0].second, uuid, "reader value");
  checkEq(h[1].first, std::string("X-Lamina-Firmware"), "firmware name");
  checkEq(h[1].second, std::string("1.6.5-x4pro"), "firmware value");
  checkEq(h[2].first, std::string("X-Lamina-Battery"), "battery name");
  checkEq(h[2].second, std::string("83"), "battery value");

  const auto none = headers("", "1.0", 50);
  checkEq(none.size(), size_t{2}, "no identity: no reader header");
  checkEq(none[0].first, std::string("X-Lamina-Firmware"), "the other two are still sent");
  const auto bad = headers("not-a-uuid", "1.0", 50);
  checkEq(bad.size(), size_t{2}, "a text that is not a UUID is never sent");
  const auto low = headers(uuid, "1.0", -3);
  checkEq(low[2].second, std::string("0"), "battery clamped low");
  const auto high = headers(uuid, "1.0", 250);
  checkEq(high[2].second, std::string("100"), "battery clamped high");
  const auto odd = headers(uuid, "bad\r\nfirmware", 1);
  checkEq(odd[1].second, std::string("badfirmware"), "the firmware value is cleaned");
  for (const auto& header : h)
    check(header.second.find('\n') == std::string::npos && header.second.find('\r') == std::string::npos &&
              !header.second.empty() && header.second.size() <= 40,
          "every value is one line of 1 to 40 characters");
}

// The Worker's own patterns for these headers (cloud/src/worker.js: READER_UUID, FIRMWARE, BATTERY): whatever the
// reader sends must pass them, or the Worker silently keeps the old value.
void testTheWorkerAcceptsWhatIsSent() {
  const std::regex readerPattern("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$");
  const std::regex firmwarePattern("^[\\x20-\\x7e]{1,40}$");
  const std::regex batteryPattern("^(?:100|[1-9]?[0-9])$");
  FakeRandom random;
  const char* versions[] = {
      "1.6.5-x4pro",  "",          "   ", "a very long firmware text that goes on and on past forty",
      "bad\r\nvalue", "v\xC3\xA9", "\x01"};
  for (int round = 0; round < 40; ++round) {
    FakeNvs nvs;
    const std::string uuid = resolve(nvs, random).uuid;
    for (const char* version : versions) {
      for (int battery : {-7, 0, 1, 9, 10, 99, 100, 101, 5000}) {
        const auto h = headers(uuid, version, battery);
        checkEq(h.size(), size_t{3}, "all three headers are sent");
        check(std::regex_match(h[0].second, readerPattern), "the Worker's reader pattern accepts it");
        check(std::regex_match(h[1].second, firmwarePattern), "the Worker's firmware pattern accepts it");
        check(std::regex_match(h[2].second, batteryPattern), "the Worker's battery pattern accepts it");
      }
    }
  }
}

void testCardFile() {
  const std::string uuid = "12345678-9abc-4ef0-9234-56789abcdef0";
  const std::string text = cardJson(info(uuid, "1.6.5-x4c+2a6befc"));
  checkEq(text,
          std::string("{\"reader_uuid\":\"12345678-9abc-4ef0-9234-56789abcdef0\",\"firmware\":\"1.6.5-x4c+2a6befc\","
                      "\"schema\":2,\"model\":\"x4c\",\"model_name\":\"Xteink X4 Classic\","
                      "\"station_mac\":\"AA:BB:CC:DD:EE:FF\",\"hotspot_mac\":\"AA:BB:CC:DD:EE:F0\","
                      "\"wifi_bands\":[\"2.4GHz\"],\"capabilities\":[\"keys\",\"method\",\"notes\",\"almanac\","
                      "\"step_timers\",\"timed_wake\",\"scheduled_downloads\",\"usb_drive\",\"usb_setup\","
                      "\"wifi_open\",\"wifi_psk\",\"wifi_eap_tls\",\"register\"]}\n"),
          "the file as the contract shows it, for an X4 Classic");
  check(cardMatches(text, info(uuid, "1.6.5-x4c+2a6befc")), "it matches itself");
  check(cardMatches(text.substr(0, text.size() - 1), info(uuid, "1.6.5-x4c+2a6befc")), "without the newline too");
  check(!cardMatches(text, info("00000000-0000-4000-8000-000000000000", "1.6.5-x4c+2a6befc")),
        "another UUID does not match");
  check(!cardMatches(text, info(uuid, "1.6.6-x4c+2a6befc")), "another firmware does not match");
  check(!cardMatches("", info(uuid, "1.6.5")), "empty");
  check(!cardMatches("{", info(uuid, "1.6.5")), "torn");
  check(!cardMatches("[]", info(uuid, "1.6.5")), "not an object");
  check(!cardMatches(std::string(2000, ' ') + text, info(uuid, "1.6.5-x4c+2a6befc")), "far too large");
  // Quotes and backslashes in the firmware survive the round trip.
  check(cardMatches(cardJson(info(uuid, "a\"b\\c")), info(uuid, "a\"b\\c")), "an awkward firmware round-trips");
  // A firmware that needs cleaning matches its cleaned form.
  check(cardMatches(cardJson(info(uuid, "  1.0  ")), info(uuid, "1.0")), "the cleaned firmware is what is written");
  check(cardMatches(cardJson(info(uuid, "")), info(uuid, "")),
        "an empty firmware is written as unknown and still matches");
}

void testMacText() {
  const uint8_t mac[MAC_BYTES] = {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  checkEq(macText(mac), std::string("AA:BB:CC:DD:EE:FF"), "uppercase, colon-separated");
  const uint8_t low[MAC_BYTES] = {0x0a, 0x00, 0x01, 0x10, 0x9f, 0x07};
  checkEq(macText(low), std::string("0A:00:01:10:9F:07"), "two digits each, zeros kept");
  check(std::regex_match(macText(low), std::regex("^([0-9A-F]{2}:){5}[0-9A-F]{2}$")), "17 characters of the form");
}

void testCardSchema2() {
  const std::string uuid = "12345678-9abc-4ef0-9234-56789abcdef0";
  const CardInfo classic = info(uuid, "1.6.5-x4c+2a6befc");
  const CardInfo pro = info(uuid, "1.6.5-x4pro+2a6befc", plaque::Model::X4Pro);

  // What each board says it can do: touch and frontlight only on the Pro.
  CardInfo parsed;
  check(parseCard(cardJson(pro), parsed), "the Pro's file parses");
  check(parsed.schema == 2 && parsed.model == "x4pro" && parsed.modelName == "Xteink X4 Pro", "the Pro's model");
  check(
      std::find(parsed.capabilities.begin(), parsed.capabilities.end(), "touch") != parsed.capabilities.end() &&
          std::find(parsed.capabilities.begin(), parsed.capabilities.end(), "frontlight") != parsed.capabilities.end(),
      "the Pro lists touch and frontlight");
  check(parseCard(cardJson(classic), parsed), "the Classic's file parses");
  check(parsed.model == "x4c" && parsed.wifiBands == std::vector<std::string>{"2.4GHz"},
        "the Classic's model and band");
  check(
      std::find(parsed.capabilities.begin(), parsed.capabilities.end(), "touch") == parsed.capabilities.end() &&
          std::find(parsed.capabilities.begin(), parsed.capabilities.end(), "frontlight") == parsed.capabilities.end(),
      "the Classic lists neither");
  check(!cardMatches(cardJson(pro), classic), "a Pro's file on a Classic's card is rewritten");

  // The two addresses have their own keys: the station one is what Wi-Fi registration needs.
  const std::string text = cardJson(classic);
  check(text.find("\"station_mac\":\"AA:BB:CC:DD:EE:FF\"") != std::string::npos, "station_mac");
  check(text.find("\"hotspot_mac\":\"AA:BB:CC:DD:EE:F0\"") != std::string::npos, "hotspot_mac");
  const CardInfo swapped =
      info(uuid, "1.6.5-x4c+2a6befc", plaque::Model::X4Classic, "AA:BB:CC:DD:EE:F0", "AA:BB:CC:DD:EE:FF");
  check(!cardMatches(cardJson(swapped), classic), "a file with the two addresses swapped is rewritten");
  check(!cardMatches(text, info(uuid, "1.6.5-x4c+2a6befc", plaque::Model::X4Classic, "AA:BB:CC:DD:EE:00")),
        "another station address is rewritten");

  // An address the chip would not give is left out, and then must be absent from the file too.
  const CardInfo noAddress = info(uuid, "1.6.5-x4c+2a6befc", plaque::Model::X4Classic, "", "");
  const std::string bare = cardJson(noAddress);
  check(bare.find("station_mac") == std::string::npos && bare.find("hotspot_mac") == std::string::npos,
        "no address: no field");
  check(cardMatches(bare, noAddress), "and it matches itself");
  check(!cardMatches(text, noAddress), "a file with addresses the reader cannot vouch for is rewritten");

  // Old files still parse; every field missing or changed makes the file out of date.
  const std::string schema1 = "{\"reader_uuid\":\"" + uuid + "\",\"firmware\":\"1.6.5-x4c+2a6befc\"}\n";
  check(parseCard(schema1, parsed), "a schema 1 file parses");
  check(parsed.schema == 1 && parsed.uuid == uuid && parsed.firmware == "1.6.5-x4c+2a6befc" && parsed.model.empty() &&
            parsed.stationMac.empty() && parsed.capabilities.empty(),
        "as schema 1 with the new fields empty");
  check(!cardMatches(schema1, classic), "a schema 1 file is out of date");
  const char* fields[] = {"schema", "model", "model_name", "station_mac", "hotspot_mac", "wifi_bands", "capabilities"};
  for (const char* field : fields) {
    // The same file without one field.
    JsonDocument doc;
    check(!deserializeJson(doc, text), "the file is JSON");
    check(doc[field].is<JsonVariant>() && !doc[field].isNull(), std::string("the file has ") + field);
    doc.remove(field);
    std::string missing;
    serializeJson(doc, missing);
    check(parseCard(missing, parsed), std::string("a file without ") + field + " still parses");
    check(!cardMatches(missing, classic), std::string("a file without ") + field + " is rewritten");
  }
  check(!cardMatches(cardJson(info(uuid, "1.6.5-x4c+2a6befc", plaque::Model::X4Pro)), classic),
        "another model is rewritten");
  CardInfo fewer = classic;
  fewer.capabilities.pop_back();
  check(!cardMatches(cardJson(fewer), classic), "a capability less is rewritten");
  CardInfo more = classic;
  more.capabilities.push_back("teleport");
  check(!cardMatches(cardJson(more), classic), "an unknown capability is not this reader's: rewritten");
  CardInfo schema3 = classic;
  std::string later = cardJson(schema3);
  later.replace(later.find("\"schema\":2"), 10, "\"schema\":3");
  check(parseCard(later, parsed) && parsed.schema == 3, "a later schema parses");
  check(!cardMatches(later, classic), "and is written back as this reader's schema");

  // Unknown fields are tolerated: a file that says all of this and more is in sync.
  std::string extra = text;
  extra.insert(extra.size() - 2, ",\"note\":{\"by\":\"someone\"},\"bands\":5");
  check(parseCard(extra, parsed), "unknown fields parse");
  check(cardMatches(extra, classic), "and do not make the file out of date");
  // Fields of the wrong type count as missing.
  check(
      parseCard("{\"reader_uuid\":5,\"schema\":\"2\",\"capabilities\":\"keys\",\"wifi_bands\":[1,\"2.4GHz\"]}", parsed),
      "wrong types parse");
  check(parsed.uuid.empty() && parsed.schema == 1 && parsed.capabilities.empty() &&
            parsed.wifiBands == std::vector<std::string>{"2.4GHz"},
        "as missing, keeping the strings of a list");

  // Room to spare: the Pro's file with the longest firmware the Worker accepts fits what is read back.
  const CardInfo longest = info(uuid, std::string(40, 'v'), plaque::Model::X4Pro);
  check(cardJson(longest).size() < 512, "a full Pro file is under 512 bytes");
  check(cardJson(longest).size() < CARD_MAX_BYTES, "and well inside what is read back");
}

// Every boot: an old file is upgraded once, then left alone.
void testUpgradeOnBoot() {
  FakeNvs nvs;
  FakeRandom random;
  FakeCard card;
  const Identity id = resolve(nvs, random);
  card.present = true;
  card.text = "{\"reader_uuid\":\"" + id.uuid + "\",\"firmware\":\"1.6.5-x4pro+1ac8494\"}\n";
  const CardInfo now = info(id.uuid, "1.6.5-x4pro+1ac8494", plaque::Model::X4Pro);
  check(mirror(card, now) == Mirror::Written, "the first boot with schema 2 upgrades a schema 1 file");
  check(cardMatches(card.text, now), "which now says everything");
  card.writes = 0;
  check(mirror(card, now) == Mirror::InSync && card.writes == 0, "the next boot reads, compares and writes nothing");
  check(mirror(card, now) == Mirror::InSync && card.writes == 0, "and the one after");
  // A card moved from an X4 Pro to an X4 Classic: the Classic's identity and model replace the Pro's.
  FakeNvs other;
  const Identity classicId = resolve(other, random);
  const CardInfo classic = info(classicId.uuid, "1.6.5-x4c+1ac8494");
  check(mirror(card, classic) == Mirror::Written, "another reader's file is replaced");
  CardInfo parsed;
  check(parseCard(card.text, parsed) && parsed.model == "x4c" && parsed.uuid == classicId.uuid, "by this reader's");
}

void testFirstBoot() {
  FakeNvs nvs;
  FakeRandom random;
  const Identity id = resolve(nvs, random);
  check(id.source == Identity::Source::Created, "first boot creates");
  check(validUuid(id.uuid), "a valid v4");
  check(nvs.stored && nvs.writes == 1 && random.draws == 1, "stored once, drawn once");
  checkEq(uuidText(nvs.value), id.uuid, "what is stored is what is used");
  check(((nvs.value[6] >> 4) == 4) && ((nvs.value[8] >> 6) == 2), "the stored bytes carry version and variant");

  const Identity again = resolve(nvs, random);
  check(again.source == Identity::Source::Stored, "the next boot reads it");
  checkEq(again.uuid, id.uuid, "the same UUID");
  checkEq(nvs.writes, 1, "and writes nothing");
  checkEq(random.draws, 1, "and draws nothing");
}

void testWhatSurvives() {
  FakeNvs nvs;
  FakeRandom random;
  FakeCard card;
  const Identity first = resolve(nvs, random);
  check(mirror(card, info(first.uuid, "1.6.5-x4pro")) == Mirror::Written, "the first mirror writes");

  // A card swap: a new, empty card. NVS is untouched, so the UUID is the same; the file is written again.
  FakeCard newCard;
  const Identity afterSwap = resolve(nvs, random);
  checkEq(afterSwap.uuid, first.uuid, "a card swap keeps the UUID");
  check(mirror(newCard, info(afterSwap.uuid, "1.6.5-x4pro")) == Mirror::Written, "and writes the file to the new card");
  check(cardMatches(newCard.text, info(first.uuid, "1.6.5-x4pro")), "the new card says the same UUID");

  // An app reflash: the app partition changes, NVS does not. The firmware text is new; the file is rewritten for it.
  const Identity afterFlash = resolve(nvs, random);
  checkEq(afterFlash.uuid, first.uuid, "an app reflash keeps the UUID");
  check(mirror(newCard, info(afterFlash.uuid, "1.7.0-x4pro")) == Mirror::Written,
        "and the file learns the new firmware");
  check(mirror(newCard, info(afterFlash.uuid, "1.7.0-x4pro")) == Mirror::InSync, "then stays put");

  // A full erase: NVS is gone. A new UUID, and the file follows NVS.
  nvs.erase();
  const Identity afterErase = resolve(nvs, random);
  check(afterErase.source == Identity::Source::Created, "a full erase creates a new identity");
  check(afterErase.uuid != first.uuid, "a different UUID");
  check(mirror(newCard, info(afterErase.uuid, "1.7.0-x4pro")) == Mirror::Written,
        "the file follows NVS, not the other way");
  check(cardMatches(newCard.text, info(afterErase.uuid, "1.7.0-x4pro")), "and says the new UUID");
}

void testNvsIsTheSourceOfTruth() {
  FakeNvs nvs;
  FakeRandom random;
  FakeCard card;
  const Identity id = resolve(nvs, random);
  // The file claims another UUID (copied from another reader's card, or edited): NVS wins, the file is rewritten.
  card.present = true;
  card.text = cardJson(info("00000000-0000-4000-8000-000000000000", "1.6.5-x4pro"));
  check(mirror(card, info(id.uuid, "1.6.5-x4pro")) == Mirror::Written, "a file with another UUID is rewritten");
  check(cardMatches(card.text, info(id.uuid, "1.6.5-x4pro")), "to say NVS's");
  checkEq(resolve(nvs, random).uuid, id.uuid, "and NVS did not change");
  // Garbage in the file.
  card.text = "not json";
  check(mirror(card, info(id.uuid, "1.6.5-x4pro")) == Mirror::Written, "garbage is rewritten");
  // A file that cannot be read is rewritten, not trusted.
  card.failRead = true;
  card.writes = 0;
  check(mirror(card, info(id.uuid, "1.6.5-x4pro")) == Mirror::Written && card.writes == 1,
        "an unreadable file is rewritten");
  card.failRead = false;
  // In sync: no write at all (the card is not worn by every boot).
  card.writes = 0;
  check(mirror(card, info(id.uuid, "1.6.5-x4pro")) == Mirror::InSync, "in sync");
  checkEq(card.writes, 0, "nothing is written when the file is right");
  // A card that refuses the write: reported, and the identity is unaffected.
  card.failWrite = true;
  check(mirror(card, info(id.uuid, "1.9.9")) == Mirror::Failed, "a refused write is reported");
  checkEq(resolve(nvs, random).uuid, id.uuid, "the identity does not depend on the card");
}

void testNvsFailures() {
  {
    // NVS cannot be read: no identity, and nothing is created over what may be there.
    FakeNvs nvs;
    nvs.stored = true;
    nvs.useForced = true;
    nvs.forced = KeyPort::Read::Failed;
    FakeRandom random;
    const Identity id = resolve(nvs, random);
    check(id.source == Identity::Source::Unavailable && id.uuid.empty(), "a failed read gives no identity");
    checkEq(nvs.writes, 0, "and writes nothing");
    checkEq(random.draws, 0, "and draws nothing");
    check(headers(id.uuid, "1.0", 50).size() == 2, "so no reader header is sent");
  }
  {
    // NVS cannot be written: the new UUID would change on the next boot, so it is not used at all.
    FakeNvs nvs;
    nvs.failWrite = true;
    FakeRandom random;
    const Identity id = resolve(nvs, random);
    check(id.source == Identity::Source::Unavailable && id.uuid.empty(), "an unwritable NVS gives no identity");
    checkEq(nvs.writes, 1, "one write was tried");
  }
  {
    // A value of the wrong size is replaced.
    FakeNvs nvs;
    nvs.stored = true;
    nvs.storedSize = 20;
    FakeRandom random;
    const Identity id = resolve(nvs, random);
    check(id.source == Identity::Source::Created && validUuid(id.uuid), "a value of the wrong size is replaced");
    checkEq(nvs.storedSize, UUID_BYTES, "by a good one");
  }
  {
    // 16 bytes that are not a v4 UUID were not stored by this code: replaced.
    FakeNvs nvs;
    nvs.stored = true;
    std::memset(nvs.value, 0x00, UUID_BYTES);
    FakeRandom random;
    const Identity id = resolve(nvs, random);
    check(id.source == Identity::Source::Created && validUuid(id.uuid), "16 bytes that are not a v4 UUID are replaced");
    check(id.uuid != "00000000-0000-0000-0000-000000000000", "not echoed back");
  }
  {
    // A failure is not cached: the next call can succeed.
    FakeNvs nvs;
    nvs.useForced = true;
    nvs.forced = KeyPort::Read::Failed;
    FakeRandom random;
    check(resolve(nvs, random).uuid.empty(), "unavailable at first");
    nvs.useForced = false;
    check(validUuid(resolve(nvs, random).uuid), "and fine once NVS answers");
  }
}

void testTwoReadersDiffer() {
  std::set<std::string> seen;
  FakeRandom random;
  for (int i = 0; i < 50; ++i) {
    FakeNvs nvs;
    seen.insert(resolve(nvs, random).uuid);
  }
  checkEq(seen.size(), size_t{50}, "readers draw different UUIDs");
}
}  // namespace

int main() {
  testUuidV4();
  testValidUuid();
  testFirmwareText();
  testHeaders();
  testTheWorkerAcceptsWhatIsSent();
  testCardFile();
  testMacText();
  testCardSchema2();
  testUpgradeOnBoot();
  testFirstBoot();
  testWhatSurvives();
  testNvsIsTheSourceOfTruth();
  testNvsFailures();
  testTwoReadersDiffer();
  std::cout << "reader identity: " << checks << " checks passed\n";
  return 0;
}
