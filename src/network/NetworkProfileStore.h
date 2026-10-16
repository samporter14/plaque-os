#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "NetworkProfile.h"

// The one installed network profile, in NVS (namespace "plaque_net"), never on the card. Two slots and a pointer:
// a new profile is validated in RAM, written to the slot the pointer does not name, read back and checked, and only
// then does the pointer move to it (a single NVS write, which NVS makes atomic); the old slot is erased last. Any
// failure on the way, NVS running out of space included, leaves the pointer and the old slot as they were and erases
// what was written. Each slot carries a SHA-256 over all of its parts, so a damaged slot reads as no profile.
//
// Layout of slot X (X is "a" or "b"): X.m the description (no secrets, with the digest), X.p the PSK, X.c the CA
// certificates (DER, back to back), X.t the client certificate (DER), X.k the client key (DER). "active" is 0 (none),
// 1 (a) or 2 (b). DER is kept rather than PEM: about three quarters of the size, and NVS space is what limits.
//
// "Erase" means what nvs_erase_key() does: the entry is marked deleted, which is logical. The bytes (a PSK, a private
// key) stay in the flash page until NVS reclaims that page, and flash encryption and NVS encryption are off, so a
// deleted profile is gone from every API of the reader but not scrubbed from the chip. Nothing here claims otherwise
// (SPEC section 4).
//
// Space: the NVS partition is 0x5000 bytes, five 4 KB pages of 126 entries of 32 bytes; NVS keeps one page free for
// itself, so 504 entries (16 128 bytes) hold everything the reader keeps in NVS. A blob takes two entries plus one
// per 32 bytes (and one more for each page it spills onto). A profile may use at most MAX_STORED_BYTES, and an install
// goes ahead only if nvs_get_stats() reports room for the new slot plus RESERVED_ENTRIES for the rest of the reader
// (the PHY calibration blob alone is about 60 entries when it is rewritten). While both slots exist, an open or PSK
// profile (about 15 entries) always fits beside another; two of the largest EAP-TLS profiles do not, and replacing
// one then needs Remove first: the install fails with storage_full and the old profile stays.
namespace netprofile {

constexpr char NVS_NAMESPACE[] = "plaque_net";
constexpr size_t NVS_ENTRY_BYTES = 32;
constexpr size_t RESERVED_ENTRIES = 72;
constexpr size_t MAX_STORED_BYTES = 8192;

enum class NvsResult : uint8_t { Ok, NotFound, NoSpace, Failed };

// The NVS namespace, as the store needs it. The device implements it over nvs_*; the host tests with a fake that
// can fail any step.
class NvsPort {
 public:
  virtual ~NvsPort() = default;
  virtual NvsResult getU8(const char* key, uint8_t& value) = 0;
  virtual NvsResult setU8(const char* key, uint8_t value) = 0;
  virtual NvsResult blobSize(const char* key, size_t& size) = 0;
  // Reads a blob of exactly `size` bytes.
  virtual NvsResult getBlob(const char* key, uint8_t* out, size_t size) = 0;
  virtual NvsResult setBlob(const char* key, const uint8_t* data, size_t size) = 0;
  virtual NvsResult erase(const char* key) = 0;  // NotFound counts as done
  virtual NvsResult commit() = 0;
  // Entries free for data in the whole partition (nvs_stats_t::available_entries); false when unknown.
  virtual bool availableEntries(size_t& entries) = 0;
};

// NVS entries one blob of `bytes` takes.
size_t blobEntries(size_t bytes);

class Store {
 public:
  enum class Read : uint8_t { None, Ok, Corrupt, Failed };

  explicit Store(NvsPort& nvs) : nvs_(nvs) {}

  // The installed profile, secrets included (for a join). Corrupt: the active slot does not check out.
  Read read(Profile& out);
  // The installed profile's public part only (the secrets are read to check the slot, then wiped).
  Read readSummary(Summary& out);
  // All or nothing (see above). Error::None, StorageFull or StorageFailed.
  Error install(const Profile& profile);
  // Moves the pointer to none, then deletes both slots (logically: see the note above). False only when the pointer
  // could not be moved, in which case the profile is still installed.
  bool remove();

  // What install() needs for a profile, in NVS entries; 0 when it is larger than MAX_STORED_BYTES.
  static size_t entriesNeeded(const Profile& profile);
  static size_t storedBytes(const Profile& profile);

 private:
  NvsPort& nvs_;

  NvsResult activeSlot(uint8_t& slot);
  bool eraseSlot(uint8_t slot);
  NvsResult writeSlot(uint8_t slot, const Profile& profile, const std::vector<uint8_t>& meta);
  Read readSlot(uint8_t slot, Profile& out);
};

}  // namespace netprofile
