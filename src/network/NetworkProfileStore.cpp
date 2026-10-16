#include "NetworkProfileStore.h"

#include <mbedtls/sha256.h>

#include <cstring>
#include <string>

namespace netprofile {
namespace {

constexpr char ACTIVE_KEY[] = "active";
constexpr uint8_t META_MAGIC[4] = {'P', 'N', 'M', 1};
constexpr size_t MAX_META_BYTES = 4096;
constexpr uint8_t FLAG_HIDDEN = 1;
constexpr uint8_t FLAG_DHCP_TIME = 2;
constexpr uint8_t FLAG_CERT_DATES = 4;

enum class Part : uint8_t { Meta, Psk, Ca, Cert, Key };

// "a.m", "b.k" ...: short enough for NVS (15 characters).
void keyFor(const uint8_t slot, const Part part, char out[4]) {
  constexpr char LETTERS[] = {'m', 'p', 'c', 't', 'k'};
  out[0] = slot == 1 ? 'a' : 'b';
  out[1] = '.';
  out[2] = LETTERS[static_cast<uint8_t>(part)];
  out[3] = '\0';
}

class Writer {
 public:
  std::vector<uint8_t> bytes;
  void u8(const uint8_t v) { bytes.push_back(v); }
  void u16(const uint16_t v) {
    bytes.push_back(static_cast<uint8_t>(v));
    bytes.push_back(static_cast<uint8_t>(v >> 8));
  }
  void i64(const int64_t v) {
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(static_cast<uint64_t>(v) >> (8 * i)));
  }
  void raw(const uint8_t* data, const size_t size) { bytes.insert(bytes.end(), data, data + size); }
  bool text(const std::string& s) {
    if (s.size() > 0xFFFF) return false;
    u16(static_cast<uint16_t>(s.size()));
    raw(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return true;
  }
};

class Reader {
  const uint8_t* data_;
  size_t size_;
  size_t pos_ = 0;

 public:
  Reader(const uint8_t* data, const size_t size) : data_(data), size_(size) {}
  bool u8(uint8_t& v) {
    if (pos_ + 1 > size_) return false;
    v = data_[pos_++];
    return true;
  }
  bool u16(uint16_t& v) {
    if (pos_ + 2 > size_) return false;
    v = static_cast<uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
    pos_ += 2;
    return true;
  }
  bool i64(int64_t& v) {
    if (pos_ + 8 > size_) return false;
    uint64_t u = 0;
    for (int i = 0; i < 8; ++i) u |= static_cast<uint64_t>(data_[pos_ + i]) << (8 * i);
    v = static_cast<int64_t>(u);
    pos_ += 8;
    return true;
  }
  bool raw(uint8_t* out, const size_t size) {
    if (pos_ + size > size_) return false;
    std::memcpy(out, data_ + pos_, size);
    pos_ += size;
    return true;
  }
  bool text(std::string& s) {
    uint16_t length = 0;
    if (!u16(length) || pos_ + length > size_) return false;
    s.assign(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return true;
  }
  size_t position() const { return pos_; }
  bool done() const { return pos_ == size_; }
};

struct Lengths {
  uint16_t psk = 0, ca = 0, cert = 0, key = 0;
};

class Sha256 {
  mbedtls_sha256_context context_;

 public:
  Sha256() {
    mbedtls_sha256_init(&context_);
    mbedtls_sha256_starts(&context_, 0);
  }
  ~Sha256() { mbedtls_sha256_free(&context_); }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;
  void update(const uint8_t* data, const size_t size) {
    if (size) mbedtls_sha256_update(&context_, data, size);
  }
  void finish(uint8_t out[32]) { mbedtls_sha256_finish(&context_, out); }
};

// The slot's digest: the description up to the digest itself, then every part.
void slotDigest(const uint8_t* meta, const size_t metaBytes, const Profile& p, uint8_t out[32]) {
  Sha256 sha;
  sha.update(meta, metaBytes);
  sha.update(p.psk.data(), p.psk.size());
  sha.update(p.caDer.data(), p.caDer.size());
  sha.update(p.clientCertDer.data(), p.clientCertDer.size());
  sha.update(p.keyDer.data(), p.keyDer.size());
  sha.finish(out);
}

bool fits16(const size_t size) { return size <= 0xFFFF; }

bool serializeMeta(const Profile& p, std::vector<uint8_t>& out) {
  const Summary& s = p.info;
  if (!fits16(p.psk.size()) || !fits16(p.caDer.size()) || !fits16(p.clientCertDer.size()) || !fits16(p.keyDer.size()) ||
      s.timeServers.size() > MAX_TIME_SERVERS || s.caSha256.size() > MAX_CA_CERTS)
    return false;
  Writer w;
  w.raw(META_MAGIC, sizeof(META_MAGIC));
  w.u8(static_cast<uint8_t>(s.auth));
  w.u8(static_cast<uint8_t>(s.institution));
  w.u8(static_cast<uint8_t>(s.category));
  w.u8(static_cast<uint8_t>((s.hidden ? FLAG_HIDDEN : 0) | (s.useDhcpTime ? FLAG_DHCP_TIME : 0) |
                            (s.hasCertDates ? FLAG_CERT_DATES : 0)));
  w.u8(static_cast<uint8_t>(s.installedVia));
  w.u8(static_cast<uint8_t>(p.keyFormat));
  w.i64(s.createdAt);
  w.i64(s.clientCertNotBefore);
  w.i64(s.clientCertNotAfter);
  if (!w.text(s.profileId) || !w.text(s.name) || !w.text(s.ssid) || !w.text(p.identity) || !w.text(s.serverDomain) ||
      !w.text(s.clientCertSubject))
    return false;
  w.u8(static_cast<uint8_t>(s.timeServers.size()));
  for (const auto& server : s.timeServers)
    if (!w.text(server)) return false;
  w.u8(static_cast<uint8_t>(s.caSha256.size()));
  for (const auto& digest : s.caSha256) w.raw(digest.bytes, sizeof(digest.bytes));
  w.u16(static_cast<uint16_t>(p.psk.size()));
  w.u16(static_cast<uint16_t>(p.caDer.size()));
  w.u16(static_cast<uint16_t>(p.clientCertDer.size()));
  w.u16(static_cast<uint16_t>(p.keyDer.size()));
  uint8_t digest[32];
  slotDigest(w.bytes.data(), w.bytes.size(), p, digest);
  w.raw(digest, sizeof(digest));
  if (w.bytes.size() > MAX_META_BYTES) return false;
  out = std::move(w.bytes);
  return true;
}

// The description of a slot. `digestAt` is where the stored digest starts.
bool parseMeta(const std::vector<uint8_t>& bytes, Profile& p, Lengths& lengths, size_t& digestAt) {
  Reader r(bytes.data(), bytes.size());
  uint8_t magic[4] = {};
  uint8_t auth = 0, institution = 0, category = 0, flags = 0, via = 0, keyFormat = 0;
  if (!r.raw(magic, sizeof(magic)) || std::memcmp(magic, META_MAGIC, sizeof(magic)) != 0) return false;
  if (!r.u8(auth) || !r.u8(institution) || !r.u8(category) || !r.u8(flags) || !r.u8(via) || !r.u8(keyFormat))
    return false;
  if (auth > static_cast<uint8_t>(Auth::EapTls) || institution > static_cast<uint8_t>(Institution::Other) ||
      category > static_cast<uint8_t>(Category::EnterpriseCertificate) || via > static_cast<uint8_t>(Source::Card) ||
      keyFormat > static_cast<uint8_t>(KeyFormat::Sec1))
    return false;
  Summary& s = p.info;
  s.auth = static_cast<Auth>(auth);
  s.institution = static_cast<Institution>(institution);
  s.category = static_cast<Category>(category);
  s.hidden = flags & FLAG_HIDDEN;
  s.useDhcpTime = flags & FLAG_DHCP_TIME;
  s.hasCertDates = flags & FLAG_CERT_DATES;
  s.installedVia = static_cast<Source>(via);
  p.keyFormat = static_cast<KeyFormat>(keyFormat);
  if (!r.i64(s.createdAt) || !r.i64(s.clientCertNotBefore) || !r.i64(s.clientCertNotAfter)) return false;
  if (!r.text(s.profileId) || !r.text(s.name) || !r.text(s.ssid) || !r.text(p.identity) || !r.text(s.serverDomain) ||
      !r.text(s.clientCertSubject))
    return false;
  uint8_t count = 0;
  if (!r.u8(count) || count > MAX_TIME_SERVERS) return false;
  s.timeServers.resize(count);
  for (auto& server : s.timeServers)
    if (!r.text(server)) return false;
  if (!r.u8(count) || count > MAX_CA_CERTS) return false;
  s.caSha256.resize(count);
  for (auto& digest : s.caSha256)
    if (!r.raw(digest.bytes, sizeof(digest.bytes))) return false;
  if (!r.u16(lengths.psk) || !r.u16(lengths.ca) || !r.u16(lengths.cert) || !r.u16(lengths.key)) return false;
  digestAt = r.position();
  uint8_t digest[32];
  return r.raw(digest, sizeof(digest)) && r.done();
}

}  // namespace

size_t blobEntries(const size_t bytes) {
  // Two overhead entries and one per 32 bytes; a blob longer than what is left of a page is split into chunks,
  // each with a header of its own (a page holds 126 entries, 4032 bytes).
  return 2 + (bytes + NVS_ENTRY_BYTES - 1) / NVS_ENTRY_BYTES + bytes / 3968;
}

size_t Store::storedBytes(const Profile& p) {
  std::vector<uint8_t> meta;
  const size_t metaBytes = serializeMeta(p, meta) ? meta.size() : MAX_META_BYTES;
  return metaBytes + p.psk.size() + p.caDer.size() + p.clientCertDer.size() + p.keyDer.size();
}

size_t Store::entriesNeeded(const Profile& p) {
  std::vector<uint8_t> meta;
  if (!serializeMeta(p, meta) || storedBytes(p) > MAX_STORED_BYTES) return 0;
  size_t entries = blobEntries(meta.size());
  for (const size_t part : {p.psk.size(), p.caDer.size(), p.clientCertDer.size(), p.keyDer.size()})
    if (part) entries += blobEntries(part);
  return entries;
}

NvsResult Store::activeSlot(uint8_t& slot) {
  slot = 0;
  const NvsResult result = nvs_.getU8(ACTIVE_KEY, slot);
  if (result == NvsResult::NotFound) {
    slot = 0;
    return NvsResult::Ok;
  }
  if (result == NvsResult::Ok && slot > 2) slot = 0;  // a pointer that names no slot names nothing
  return result;
}

bool Store::eraseSlot(const uint8_t slot) {
  bool ok = true;
  for (const Part part : {Part::Meta, Part::Psk, Part::Ca, Part::Cert, Part::Key}) {
    char key[4];
    keyFor(slot, part, key);
    const NvsResult result = nvs_.erase(key);
    ok = ok && (result == NvsResult::Ok || result == NvsResult::NotFound);
  }
  return ok;
}

NvsResult Store::writeSlot(const uint8_t slot, const Profile& p, const std::vector<uint8_t>& meta) {
  struct Item {
    Part part;
    const uint8_t* data;
    size_t size;
  };
  // The description goes last: a slot without one is never read.
  const Item items[] = {{Part::Psk, p.psk.data(), p.psk.size()},
                        {Part::Ca, p.caDer.data(), p.caDer.size()},
                        {Part::Cert, p.clientCertDer.data(), p.clientCertDer.size()},
                        {Part::Key, p.keyDer.data(), p.keyDer.size()},
                        {Part::Meta, meta.data(), meta.size()}};
  for (const Item& item : items) {
    if (item.size == 0) continue;
    char key[4];
    keyFor(slot, item.part, key);
    const NvsResult result = nvs_.setBlob(key, item.data, item.size);
    if (result != NvsResult::Ok) return result;
  }
  return NvsResult::Ok;
}

Store::Read Store::readSlot(const uint8_t slot, Profile& out) {
  out = Profile();
  char key[4];
  keyFor(slot, Part::Meta, key);
  size_t metaBytes = 0;
  NvsResult result = nvs_.blobSize(key, metaBytes);
  if (result == NvsResult::NotFound) return Read::Corrupt;  // the pointer names a slot with nothing in it
  if (result != NvsResult::Ok) return Read::Failed;
  if (metaBytes == 0 || metaBytes > MAX_META_BYTES) return Read::Corrupt;
  std::vector<uint8_t> meta(metaBytes);
  if (nvs_.getBlob(key, meta.data(), meta.size()) != NvsResult::Ok) return Read::Failed;
  Lengths lengths;
  size_t digestAt = 0;
  if (!parseMeta(meta, out, lengths, digestAt)) return Read::Corrupt;

  const auto load = [&](const Part part, const size_t size, uint8_t* into) -> Read {
    if (size == 0) return Read::Ok;
    char partKey[4];
    keyFor(slot, part, partKey);
    size_t stored = 0;
    const NvsResult r = nvs_.blobSize(partKey, stored);
    if (r == NvsResult::NotFound || (r == NvsResult::Ok && stored != size)) return Read::Corrupt;
    if (r != NvsResult::Ok || nvs_.getBlob(partKey, into, size) != NvsResult::Ok) return Read::Failed;
    return Read::Ok;
  };
  if (!out.psk.allocate(lengths.psk) || !out.keyDer.allocate(lengths.key)) return Read::Failed;
  out.caDer.resize(lengths.ca);
  out.clientCertDer.resize(lengths.cert);
  for (const Read r :
       {load(Part::Psk, lengths.psk, out.psk.data()), load(Part::Ca, lengths.ca, out.caDer.data()),
        load(Part::Cert, lengths.cert, out.clientCertDer.data()), load(Part::Key, lengths.key, out.keyDer.data())}) {
    if (r != Read::Ok) {
      out = Profile();
      return r;
    }
  }
  uint8_t digest[32];
  slotDigest(meta.data(), digestAt, out, digest);
  if (std::memcmp(digest, meta.data() + digestAt, sizeof(digest)) != 0) {
    out = Profile();
    return Read::Corrupt;
  }
  out.info.installed = true;
  out.described = true;
  return Read::Ok;
}

Store::Read Store::read(Profile& out) {
  out = Profile();
  uint8_t slot = 0;
  if (activeSlot(slot) != NvsResult::Ok) return Read::Failed;
  if (slot == 0) return Read::None;
  return readSlot(slot, out);
}

Store::Read Store::readSummary(Summary& out) {
  out = Summary();
  Profile profile;
  const Read result = read(profile);
  if (result == Read::Ok) out = profile.info;
  return result;
}

Error Store::install(const Profile& profile) {
  std::vector<uint8_t> meta;
  if (!serializeMeta(profile, meta)) return Error::StorageFailed;
  if (storedBytes(profile) > MAX_STORED_BYTES) return Error::StorageFull;

  uint8_t active = 0;
  if (activeSlot(active) != NvsResult::Ok) return Error::StorageFailed;  // which slot is free is not known
  const uint8_t target = active == 1 ? 2 : 1;
  // Leftovers of an interrupted install in the target; with nothing active, of an interrupted removal in either.
  if (!eraseSlot(target) || (active == 0 && !eraseSlot(target == 1 ? 2 : 1)) || nvs_.commit() != NvsResult::Ok)
    return Error::StorageFailed;

  size_t available = 0;
  if (nvs_.availableEntries(available) && available < entriesNeeded(profile) + RESERVED_ENTRIES)
    return Error::StorageFull;

  NvsResult result = writeSlot(target, profile, meta);
  if (result == NvsResult::Ok) result = nvs_.commit();
  if (result == NvsResult::Ok) {
    Profile check;
    if (readSlot(target, check) != Read::Ok) result = NvsResult::Failed;
  }
  bool flipped = false;
  if (result == NvsResult::Ok) {
    result = nvs_.setU8(ACTIVE_KEY, target);
    if (result == NvsResult::Ok) result = nvs_.commit();
    flipped = result == NvsResult::Ok;
    if (!flipped) {
      // The pointer write reported a failure; what it says now decides which slot is the profile.
      uint8_t now = 0;
      if (activeSlot(now) != NvsResult::Ok) return Error::StorageFailed;  // unknown: leave both slots alone
      flipped = now == target;
    }
  }
  if (!flipped) {
    eraseSlot(target);
    nvs_.commit();
    return result == NvsResult::NoSpace ? Error::StorageFull : Error::StorageFailed;
  }
  if (active != 0) {
    // The old profile is no longer named by anything; a failure here leaves garbage the next install erases.
    eraseSlot(active);
    nvs_.commit();
  }
  return Error::None;
}

bool Store::remove() {
  if (nvs_.setU8(ACTIVE_KEY, 0) != NvsResult::Ok || nvs_.commit() != NvsResult::Ok) {
    uint8_t now = 0;
    if (activeSlot(now) != NvsResult::Ok || now != 0) return false;
  }
  eraseSlot(1);
  eraseSlot(2);
  nvs_.erase(ACTIVE_KEY);
  nvs_.commit();
  return true;
}

}  // namespace netprofile
