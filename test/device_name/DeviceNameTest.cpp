#include <cstdlib>
#include <iostream>
#include <regex>
#include <set>
#include <string>

#include "DeviceName.h"

// Host tests for the reader's network names (src/network/DeviceName.*): the hotspot SSID and the host name, both made
// from the last two bytes of the station MAC.

using namespace devicename;

namespace {
int checks = 0;

void check(const bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* name) {
  if (!(actual == expected)) {
    std::cerr << "FAIL: " << name << "\n  actual:   " << actual << "\n  expected: " << expected << '\n';
    std::exit(1);
  }
  ++checks;
}
}  // namespace

int main() {
  const uint8_t mac[6] = {0x34, 0x85, 0x18, 0xa1, 0xb2, 0xc3};

  // The shape: the last two bytes, uppercase for the SSID and lowercase for the host name.
  checkEq(hotspotSsid(mac), "Plaque-B2C3", "the hotspot SSID ends in the last two MAC bytes, uppercase");
  checkEq(hostname(mac), "plaque-b2c3", "the host name ends in the last two MAC bytes, lowercase");

  // The same four digits as the tail of the MAC Settings > About shows (uppercase, colon-separated).
  const std::string about = "34:85:18:A1:B2:C3";
  check(about.substr(about.size() - 5) == "B2:C3", "the About MAC ends in B2:C3");
  check(hotspotSsid(mac).substr(7) == "B2C3", "which are the SSID's four digits");

  // Leading zeros are kept, so a name is always four digits.
  const uint8_t low[6] = {0x34, 0x85, 0x18, 0xa1, 0x00, 0x0f};
  checkEq(hotspotSsid(low), "Plaque-000F", "a small last byte pair is padded");
  checkEq(hostname(low), "plaque-000f", "also in the host name");
  const uint8_t endsInZeros[6] = {0x34, 0x85, 0x18, 0xa1, 0x00, 0x00};
  checkEq(hotspotSsid(endsInZeros), "Plaque-0000", "a real MAC that ends in 00:00 is still a name with digits");

  // Two readers differ, one reader does not.
  const uint8_t other[6] = {0x34, 0x85, 0x18, 0xa1, 0xb2, 0xc4};
  check(hotspotSsid(mac) != hotspotSsid(other), "two readers have different SSIDs");
  check(hostname(mac) != hostname(other), "and different host names");
  checkEq(hotspotSsid(mac), hotspotSsid(mac), "the same MAC gives the same SSID");
  checkEq(hostname(mac), hostname(mac), "and the same host name");
  // The first four bytes do not matter: the Espressif prefix is the same on every reader.
  const uint8_t sameTail[6] = {0x01, 0x02, 0x03, 0x04, 0xb2, 0xc3};
  checkEq(hostname(sameTail), hostname(mac), "only the last two bytes are used");

  // A MAC that could not be read gives the bare name, never a made-up suffix.
  const uint8_t none[6] = {};
  checkEq(hotspotSsid(none), "Plaque", "no MAC: the SSID is the product name");
  checkEq(hostname(none), "plaque", "no MAC: the host name is the product name in lowercase");
  checkEq(hotspotSsid(nullptr), "Plaque", "a null MAC is the same");
  checkEq(hostname(nullptr), "plaque", "also for the host name");

  // What a network and a name server accept, for every possible last two bytes.
  const std::regex ssid("^Plaque-[0-9A-F]{4}$");
  const std::regex label("^[a-z0-9]([a-z0-9-]*[a-z0-9])?$");
  std::set<std::string> ssids, hosts;
  for (int hi = 0; hi < 256; hi++) {
    for (int lo = 0; lo < 256; lo++) {
      const uint8_t m[6] = {0x34, 0x85, 0x18, 0xa1, static_cast<uint8_t>(hi), static_cast<uint8_t>(lo)};
      const std::string s = hotspotSsid(m), h = hostname(m);
      ssids.insert(s);
      hosts.insert(h);
      if (!std::regex_match(s, ssid) || s.size() > SSID_MAX_CHARS || !std::regex_match(h, label) ||
          h.size() > LABEL_MAX_CHARS) {
        std::cerr << "FAIL: bad name for " << hi << ' ' << lo << ": " << s << ' ' << h << '\n';
        return 1;
      }
    }
  }
  ++checks;  // every pair above passed the pattern and length checks
  checkEq(ssids.size(), size_t{65536}, "every last-two-bytes pair has its own SSID");
  checkEq(hosts.size(), size_t{65536}, "and its own host name");
  check(std::string("Plaque").size() <= SSID_MAX_CHARS && std::string("plaque").size() <= LABEL_MAX_CHARS,
        "the bare names fit too");

  // It is not the old name.
  check(hotspotSsid(mac).find("CrossPoint") == std::string::npos, "the SSID no longer says CrossPoint");
  check(hostname(mac).find("crosspoint") == std::string::npos, "nor does the host name");

  std::cout << "device name: " << checks << " checks passed\n";
  return 0;
}
