#pragma once

#include <cstddef>

// Which SNTP servers a sync asks, decided without lwIP so the host tests can cover it (test/time_servers). TrustedTime
// applies the plan when it (re)starts SNTP.
//
// lwIP keeps CONFIG_LWIP_SNTP_MAX_SERVERS (3) server slots. A slot holds either a name (a host name or an IPv4 literal,
// resolved when the request is sent) or a bare address. When "servers from DHCP" is on, lwIP writes the NTP servers of
// a new lease as bare addresses into the first slots (at most CONFIG_LWIP_DHCP_MAX_NTP_SERVERS of them); a plan keeps
// those slots as they are and puts the configured names after them, so the network's own server is asked first.
namespace trustedtime {

constexpr size_t MAX_SERVERS = 3;         // CONFIG_LWIP_SNTP_MAX_SERVERS
constexpr size_t MAX_SERVER_CHARS = 253;  // a host name's longest text form
constexpr const char* DEFAULT_SERVERS[MAX_SERVERS] = {"pool.ntp.org", "time.google.com", "time.cloudflare.com"};

// What a sync does with one slot.
enum class SlotUse : unsigned char {
  Name,   // ask `name`
  Lease,  // keep the address the DHCP lease put there
  Clear,  // nothing: clear whatever the slot held before
};

struct ServerPlan {
  SlotUse use[MAX_SERVERS] = {SlotUse::Clear, SlotUse::Clear, SlotUse::Clear};
  const char* name[MAX_SERVERS] = {nullptr, nullptr, nullptr};  // set for SlotUse::Name
  bool defaults = false;  // exactly DEFAULT_SERVERS, in order: the plan of a reader nobody configured
};

// `configured`: the servers set with setServers(), 0 for the defaults. `leaseSlots`: how many leading slots hold a
// DHCP lease's server right now (always 0 unless DHCP servers are on).
ServerPlan planServers(const char* const* configured, size_t configuredCount, size_t leaseSlots);

// A server name setServers() accepts: 1 to MAX_SERVER_CHARS printable ASCII characters with no space. The profile
// parser checks host name and IPv4 syntax; this only keeps lwIP from being handed something unusable.
bool acceptableServerName(const char* name);

}  // namespace trustedtime
