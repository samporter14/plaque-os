#include "NetworkJoin.h"

#include <algorithm>

namespace netjoin {
namespace {
bool validLast(const Situation& s) { return s.lastUsed >= 0 && static_cast<size_t>(s.lastUsed) < s.savedCount; }
}  // namespace

int beforeScan(const Mode mode, const Situation& s) {
  if (mode == Mode::Wake && s.profile) return static_cast<int>(profileIndex(s));
  if (mode == Mode::Manual && s.profile) return -1;
  return validLast(s) ? s.lastUsed : -1;
}

std::vector<size_t> afterScan(const Mode mode, const Situation& s, const bool profileSeen,
                              const std::vector<size_t>& savedSeen) {
  std::vector<size_t> order;
  if (mode == Mode::Wake) {
    if (s.profile && validLast(s)) order.push_back(static_cast<size_t>(s.lastUsed));
    return order;
  }
  if (!s.profile) {
    for (const size_t saved : savedSeen)
      if (saved < s.savedCount) order.push_back(saved);
    return order;
  }
  order.reserve(savedSeen.size() + 2);
  if (profileSeen || s.profileHidden) order.push_back(profileIndex(s));
  if (validLast(s)) order.push_back(static_cast<size_t>(s.lastUsed));
  for (const size_t saved : savedSeen)
    if (saved < s.savedCount && std::find(order.begin(), order.end(), saved) == order.end()) order.push_back(saved);
  return order;
}

std::vector<size_t> wakeOrder(const Situation& s) {
  std::vector<size_t> order;
  if (s.profile) order.push_back(profileIndex(s));
  if (validLast(s)) order.push_back(static_cast<size_t>(s.lastUsed));
  return order;
}

namespace {
constexpr uint8_t JOINED_TAG = 0xA0;
}  // namespace

uint8_t encodeJoined(const Joined joined) { return static_cast<uint8_t>(JOINED_TAG | static_cast<uint8_t>(joined)); }

Joined decodeJoined(const uint8_t byte) {
  if ((byte & 0xF0) != JOINED_TAG) return Joined::Nobody;
  switch (byte & 0x0F) {
    case static_cast<uint8_t>(Joined::Profile):
      return Joined::Profile;
    case static_cast<uint8_t>(Joined::Saved):
      return Joined::Saved;
    default:
      break;
  }
  return Joined::Nobody;
}

std::vector<WakeStep> wakePlan(const Situation& s, const Joined last) {
  std::vector<WakeStep> plan;
  plan.reserve(2);
  const bool saved = validLast(s);
  const WakeStep profileStep{profileIndex(s), s.profileEapTls ? WAKE_JOIN_EAP_TLS_MS : WAKE_JOIN_PROFILE_MS};
  if (s.profile && saved && last == Joined::Saved) {
    plan.push_back({static_cast<size_t>(s.lastUsed), WAKE_JOIN_SAVED_MS});
    plan.push_back(profileStep);
    return plan;
  }
  if (s.profile) plan.push_back(profileStep);
  if (saved) plan.push_back({static_cast<size_t>(s.lastUsed), WAKE_JOIN_SAVED_MS});
  return plan;
}

}  // namespace netjoin
