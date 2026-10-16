#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "NetworkProfile.h"
#include "NetworkProfileStore.h"

// Import from card (SPEC-UNIVERSITY-WIFI.md section 5): the person asks for it; the reader reads
// /Lamina/Provision/network-profile.json, overwrites it with zeros and deletes it whatever the result, refuses a
// bundle that carries a private key, shows what it would install and installs only when the person says so (through
// the same validation and store as USB), and leaves /Lamina/network-import.json (no secrets) for the Mac. Pure;
// test/network_profile covers it.
//
// In two steps because the question in between is the screen's: readCard() reads, validates and wipes; finishCard()
// installs or declines and writes the result file. importFromCard() is the two together, for a caller that does not
// ask.
namespace netprofile {

constexpr char CARD_INBOX_PATH[] = "/Lamina/Provision/network-profile.json";
constexpr char CARD_RESULT_PATH[] = "/Lamina/network-import.json";
constexpr char CARD_SUMMARY_PATH[] = "/Lamina/network-profile.json";

class CardFiles {
 public:
  virtual ~CardFiles() = default;
  // The card is mounted and not handed to the computer as a USB drive.
  virtual bool available() = 0;
  // Size of the inbox file, or -1 when there is none.
  virtual int64_t inboxSize() = 0;
  // Reads exactly `size` bytes of the inbox file.
  virtual bool readInbox(uint8_t* out, size_t size) = 0;
  // Overwrites every byte of the inbox file with zeros in place (no truncation, so the same clusters are written),
  // flushes, then deletes the file.
  virtual bool wipeInbox() = 0;
  virtual bool writeResult(std::string_view json) = 0;
};

enum class CardOutcome : uint8_t { NoCard, NoFile, Installed, Rejected };

struct CardResult {
  CardOutcome outcome = CardOutcome::NoFile;
  Error error = Error::None;
  bool wiped = false;          // the inbox file was zeroed and deleted
  bool resultWritten = false;  // network-import.json was written
  Summary profile;             // what the bundle was, when it said enough (profile.installed tells)
};

// What the first step found. Pending: a bundle that validated, held in `profile` until finishCard() installs it or the
// person declines (the inbox file is gone either way). Anything else is final: no card, no file, or a bundle that was
// refused (its result file already written).
struct CardRead {
  CardOutcome outcome = CardOutcome::NoFile;
  bool pending = false;
  Error error = Error::None;
  bool wiped = false;
  bool resultWritten = false;
  Profile profile;
};

// Reads the inbox, validates it as a card bundle, wipes the file. `at` is trusted unix time for the result file, or 0.
// A refused bundle gets its result written; a good one overwrites the last result with "pending" (no secret in it) and
// the answer replaces that.
CardRead readCard(CardFiles& files, const ValidateOptions& options, int64_t at);

// The second step for a pending read: `install` true installs it (Error::None, StorageFull or StorageFailed), false
// declines (Error::Declined, nothing stored). Either way the result file is written. The profile's secrets are wiped.
CardResult finishCard(CardFiles& files, Store& store, CardRead& read, bool install, int64_t at);

// readCard() and, if the bundle is good, finishCard() with install true.
CardResult importFromCard(CardFiles& files, Store& store, const ValidateOptions& options, int64_t at);

}  // namespace netprofile
