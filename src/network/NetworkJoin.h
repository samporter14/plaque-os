#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The order in which every mailbox caller tries networks (SPEC-UNIVERSITY-WIFI.md section 6): the installed profile
// first, then the saved networks as they have always been tried. Manual checks: the last-used network, then a scan
// of the saved networks in range. Scheduled wakes: the last-used network only, never a scan.
//
// Candidates are numbered like the Method check's WifiPort: saved networks keep their WifiCredentialStore index and
// the profile is the index one past them. Pure; test/network_profile covers it.
namespace netjoin {

enum class Mode : uint8_t { Manual, Wake };

struct Situation {
  bool profile = false;        // a profile is installed
  bool profileEapTls = false;  // and it needs an EAP-TLS handshake, which takes longer than an open or PSK join
  bool profileHidden = false;  // its network does not announce itself, so a scan cannot be asked about it
  int lastUsed = -1;           // the saved network joined last, or -1
  size_t savedCount = 0;
};

inline size_t profileIndex(const Situation& s) { return s.savedCount; }
inline bool isProfile(const Situation& s, const size_t index) { return s.profile && index == s.savedCount; }
inline size_t candidateCount(const Situation& s) { return s.savedCount + (s.profile ? 1 : 0); }

// The network tried before any scan, or -1 to scan first. A manual check with a profile scans first, so that the
// profile is tried only where it is in range (or hidden) and gets a full attempt; without one, the last-used network
// first, as always. A wake tries the profile, else the last-used network.
int beforeScan(Mode mode, const Situation& s);

// Whether the check should scan at all. A wake never does.
inline bool scans(Mode mode) { return mode == Mode::Manual; }

// After the scan (or in place of it, on a wake): the rest of the order. savedSeen are the saved networks the scan
// found, strongest first. Manual with a profile: the profile if it was seen or is hidden, then the last-used network,
// then the other saved networks seen. Manual without: the saved networks seen, as before. A wake that began with the
// profile goes on to the last-used network, and stops there.
std::vector<size_t> afterScan(Mode mode, const Situation& s, bool profileSeen, const std::vector<size_t>& savedSeen);

// The whole order for a join that does not scan (a timer wake's blocking join): profile, then last used.
std::vector<size_t> wakeOrder(const Situation& s);

// ---- a timer wake's join: who first, and how long each --------------------------------------------------------------
//
// A wake used to have 10 s in all for the profile and the last-used network together, so a profile that was slow, or
// not in range, left the saved network no time (and an EAP-TLS profile, with its handshake, no chance at all). Now each
// candidate has a budget of its own, and the candidate that joined last time goes first: a reader that lives at home
// and was last at home asks home first, and one that was last on campus asks the profile first.

// Who joined the last time a wake, a shelf check or a profile join succeeded. Kept in RTC memory by the device side
// (it survives deep sleep, not a power-off), so it is a hint: anything unreadable is Nobody, and the profile goes
// first.
enum class Joined : uint8_t { Nobody, Profile, Saved };
// The RTC-memory byte for a value: a tag in the high nibble, so memory that was never written (zero) or was left
// holding something else reads as Nobody.
uint8_t encodeJoined(Joined joined);
Joined decodeJoined(uint8_t byte);

constexpr uint32_t WAKE_JOIN_SAVED_MS = 10000;    // a saved network: all that the whole wake join ever had
constexpr uint32_t WAKE_JOIN_PROFILE_MS = 10000;  // an open or PSK profile
constexpr uint32_t WAKE_JOIN_EAP_TLS_MS = 25000;  // an EAP-TLS profile: scan, EAP-TLS handshake (RSA), 4-way, DHCP
// The longest a wake's join can take with a profile (or none) installed and a saved network: each candidate's budget
// added up. steptimer::pollBudgetMs() takes it, so a step timer's alert is never late because of the join.
constexpr uint32_t wakeJoinWorstMs(const bool profile, const bool profileEapTls) {
  return WAKE_JOIN_SAVED_MS + (profile ? (profileEapTls ? WAKE_JOIN_EAP_TLS_MS : WAKE_JOIN_PROFILE_MS) : 0);
}

struct WakeStep {
  size_t candidate;  // numbered as everywhere: the saved networks, then the profile
  uint32_t budgetMs;
};
// The candidates a wake tries, in order, each with its budget. A reader with no profile gets the last-used network
// alone with 10 s, exactly as before. With a profile: the profile then the last-used network, or the other way round
// when a saved network joined last (and is still the last-used one).
std::vector<WakeStep> wakePlan(const Situation& s, Joined last);

}  // namespace netjoin
