#include "TimeServers.h"

#include <cstring>

namespace trustedtime {

ServerPlan planServers(const char* const* configured, size_t configuredCount, size_t leaseSlots) {
  ServerPlan plan;
  const bool useDefaults = configured == nullptr || configuredCount == 0;
  const char* const* names = useDefaults ? DEFAULT_SERVERS : configured;
  const size_t nameCount = useDefaults ? MAX_SERVERS : (configuredCount < MAX_SERVERS ? configuredCount : MAX_SERVERS);
  const size_t leases = leaseSlots < MAX_SERVERS ? leaseSlots : MAX_SERVERS;

  size_t slot = 0;
  for (; slot < leases; ++slot) plan.use[slot] = SlotUse::Lease;
  for (size_t i = 0; i < nameCount && slot < MAX_SERVERS; ++i, ++slot) {
    plan.use[slot] = SlotUse::Name;
    plan.name[slot] = names[i];
  }
  plan.defaults = useDefaults && leases == 0;
  return plan;
}

bool acceptableServerName(const char* name) {
  if (name == nullptr) return false;
  const size_t length = std::strlen(name);
  if (length == 0 || length > MAX_SERVER_CHARS) return false;
  for (size_t i = 0; i < length; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c <= ' ' || c > '~') return false;
  }
  return true;
}

}  // namespace trustedtime
