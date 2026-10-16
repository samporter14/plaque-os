#include "DeviceName.h"

namespace devicename {
namespace {
std::string named(const char* name, const uint8_t mac[6], const char* digits) {
  std::string out = name;
  bool unreadable = true;
  for (int i = 0; i < 6 && mac != nullptr; i++) unreadable = unreadable && mac[i] == 0;
  if (unreadable) return out;
  out += '-';
  for (const uint8_t byte : {mac[4], mac[5]}) {
    out += digits[byte >> 4];
    out += digits[byte & 0x0F];
  }
  return out;
}
}  // namespace

std::string hotspotSsid(const uint8_t mac[6]) { return named("Plaque", mac, "0123456789ABCDEF"); }

std::string hostname(const uint8_t mac[6]) { return named("plaque", mac, "0123456789abcdef"); }

}  // namespace devicename
