#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "NetworkCardImport.h"
#include "NetworkProfile.h"
#include "NetworkProfileStore.h"
#include "TestBundles.h"

using netprofile::Error;
using netprofile::NvsResult;
using netprofile::Store;
using testbundle::eapObject;
using testbundle::openBundle;
using testbundle::pki;
using testbundle::pskBundle;
using testbundle::q;

namespace {

// An NVS namespace in memory that can fail the nth write (or commit) and report how much room is left.
class FakeNvs final : public netprofile::NvsPort {
 public:
  std::map<std::string, uint8_t> u8;
  std::map<std::string, std::vector<uint8_t>> blobs;
  int writes = 0;
  int failWrite = 0;  // the nth set* call fails (1-based); 0: none
  NvsResult failWith = NvsResult::NoSpace;
  int commits = 0;
  int failCommit = 0;
  size_t available = 400;

  NvsResult getU8(const char* key, uint8_t& value) override {
    const auto it = u8.find(key);
    if (it == u8.end()) return NvsResult::NotFound;
    value = it->second;
    return NvsResult::Ok;
  }
  NvsResult setU8(const char* key, uint8_t value) override {
    if (++writes == failWrite) return failWith;
    u8[key] = value;
    return NvsResult::Ok;
  }
  NvsResult blobSize(const char* key, size_t& size) override {
    const auto it = blobs.find(key);
    if (it == blobs.end()) return NvsResult::NotFound;
    size = it->second.size();
    return NvsResult::Ok;
  }
  NvsResult getBlob(const char* key, uint8_t* out, size_t size) override {
    const auto it = blobs.find(key);
    if (it == blobs.end()) return NvsResult::NotFound;
    if (it->second.size() != size) return NvsResult::Failed;
    std::copy(it->second.begin(), it->second.end(), out);
    return NvsResult::Ok;
  }
  NvsResult setBlob(const char* key, const uint8_t* data, size_t size) override {
    if (++writes == failWrite) return failWith;
    blobs[key] = std::vector<uint8_t>(data, data + size);
    return NvsResult::Ok;
  }
  NvsResult erase(const char* key) override {
    blobs.erase(key);
    u8.erase(key);
    return NvsResult::Ok;
  }
  NvsResult commit() override { return ++commits == failCommit ? NvsResult::Failed : NvsResult::Ok; }
  bool availableEntries(size_t& entries) override {
    entries = available;
    return true;
  }
};

netprofile::ValidateOptions options() {
  netprofile::ValidateOptions o;
  o.random = testpki::random;
  o.randomContext = testpki::randomContext();
  return o;
}

netprofile::Profile make(const testbundle::Bundle& bundle) {
  netprofile::Profile profile;
  const Error error = netprofile::parseBundle(bundle.text(), options(), profile);
  EXPECT_EQ(Error::None, error);
  return profile;
}

testbundle::Bundle eap() {
  return openBundle()
      .set("auth", q("eap-tls"))
      .set("category", q("enterprise-certificate"))
      .set("eap", eapObject("plaque-0b6c9f9e@example.edu", "radius.example.edu", pki().root, pki().certRsa,
                            testpki::keyPem(pki().clientRsa)));
}

std::string pskOf(const netprofile::Profile& p) {
  return std::string(reinterpret_cast<const char*>(p.psk.data()), p.psk.size());
}

}  // namespace

TEST(NetworkStore, NothingInstalledReadsAsNone) {
  FakeNvs nvs;
  Store store(nvs);
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::None, store.read(out));
  netprofile::Summary summary;
  EXPECT_EQ(Store::Read::None, store.readSummary(summary));
}

TEST(NetworkStore, APskProfileRoundTripsWithItsSecret) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(pskBundle())));
  netprofile::Profile out;
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ(netprofile::Auth::Psk, out.info.auth);
  EXPECT_EQ("MSetup", out.info.ssid);
  EXPECT_EQ(testbundle::PSK, pskOf(out));
  netprofile::Summary summary;
  ASSERT_EQ(Store::Read::Ok, store.readSummary(summary));
  EXPECT_EQ("Lab 100 Wi-Fi", summary.name);
  EXPECT_TRUE(summary.installed);
}

TEST(NetworkStore, ACertificateProfileRoundTripsItsKeyAndCertificates) {
  FakeNvs nvs;
  Store store(nvs);
  const netprofile::Profile in = make(eap());
  ASSERT_GT(Store::entriesNeeded(in), 0u);
  ASSERT_LE(Store::storedBytes(in), netprofile::MAX_STORED_BYTES);
  ASSERT_EQ(Error::None, store.install(in));
  netprofile::Profile out;
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ(in.caDer, out.caDer);
  EXPECT_EQ(in.clientCertDer, out.clientCertDer);
  ASSERT_EQ(in.keyDer.size(), out.keyDer.size());
  EXPECT_EQ(0, std::memcmp(in.keyDer.data(), out.keyDer.data(), in.keyDer.size()));
  EXPECT_EQ(in.identity, out.identity);
  EXPECT_EQ("radius.example.edu", out.info.serverDomain);
}

TEST(NetworkStore, ReplacingLeavesOnlyTheNewProfile) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(openBundle())));
  const size_t afterFirst = nvs.blobs.size();
  ASSERT_EQ(Error::None, store.install(make(pskBundle().set("ssid", q("Lab5")))));
  netprofile::Profile out;
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ("Lab5", out.info.ssid);
  EXPECT_LE(nvs.blobs.size(), afterFirst + 1);  // the old slot was erased, not kept beside the new one
}

TEST(NetworkStore, AFailureAtAnyWriteKeepsTheWorkingProfile) {
  // Fail each write in turn, for both kinds of failure, while replacing a working profile.
  for (const NvsResult failure : {NvsResult::NoSpace, NvsResult::Failed}) {
    bool succeeded = false;
    for (int n = 1; n < 60 && !succeeded; ++n) {
      FakeNvs nvs;
      Store store(nvs);
      ASSERT_EQ(Error::None, store.install(make(pskBundle())));
      nvs.writes = 0;
      nvs.failWrite = n;
      nvs.failWith = failure;
      const Error error = store.install(make(eap()));
      nvs.failWrite = 0;
      netprofile::Profile out;
      ASSERT_EQ(Store::Read::Ok, store.read(out)) << "write " << n;
      if (error == Error::None) {
        succeeded = true;
        EXPECT_EQ(netprofile::Auth::EapTls, out.info.auth);
        continue;
      }
      EXPECT_EQ(failure == NvsResult::NoSpace ? Error::StorageFull : Error::StorageFailed, error) << "write " << n;
      EXPECT_EQ(netprofile::Auth::Psk, out.info.auth) << "write " << n << " changed the profile";
      EXPECT_EQ(testbundle::PSK, pskOf(out)) << "write " << n;
      // What the failed attempt wrote is gone again: the only certificates on offer are none.
      for (const auto& blob : nvs.blobs)
        EXPECT_EQ(std::string::npos, blob.first.find("c")) << blob.first << " at write " << n;
    }
    EXPECT_TRUE(succeeded) << "the loop never reached a write that was not needed";
  }
}

TEST(NetworkStore, AFailedCommitKeepsTheWorkingProfile) {
  for (int n = 1; n < 12; ++n) {
    FakeNvs nvs;
    Store store(nvs);
    ASSERT_EQ(Error::None, store.install(make(pskBundle())));
    nvs.commits = 0;
    nvs.failCommit = n;
    const Error error = store.install(make(openBundle().set("ssid", q("Other"))));
    nvs.failCommit = 0;
    netprofile::Profile out;
    ASSERT_EQ(Store::Read::Ok, store.read(out)) << "commit " << n;
    if (error != Error::None) {
      EXPECT_EQ("MSetup", out.info.ssid) << "commit " << n;
    } else {
      EXPECT_EQ("Other", out.info.ssid);
      break;
    }
  }
}

TEST(NetworkStore, NotEnoughRoomRefusesBeforeTouchingAnything) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(pskBundle())));
  const auto blobsBefore = nvs.blobs;
  const auto u8Before = nvs.u8;
  nvs.available = netprofile::RESERVED_ENTRIES + 3;
  EXPECT_EQ(Error::StorageFull, store.install(make(eap())));
  EXPECT_EQ(blobsBefore, nvs.blobs);
  EXPECT_EQ(u8Before, nvs.u8);
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::Ok, store.read(out));
}

TEST(NetworkStore, ADamagedSlotReadsAsCorruptNotAsAProfile) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(pskBundle())));
  for (auto& blob : nvs.blobs) {
    if (blob.first.size() > 2 && blob.first.find(".p") != std::string::npos) blob.second[0] ^= 0x01;
  }
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::Corrupt, store.read(out));
  netprofile::Summary summary;
  EXPECT_NE(Store::Read::Ok, store.readSummary(summary));
  EXPECT_FALSE(summary.installed);
}

TEST(NetworkStore, RemoveErasesTheSecretsAndLeavesNothing) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(eap())));
  ASSERT_TRUE(store.remove());
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::None, store.read(out));
  EXPECT_TRUE(nvs.blobs.empty());
}

TEST(NetworkStore, AProfileLargerThanTheCapIsNotStored) {
  netprofile::Profile big = make(openBundle());
  big.clientCertDer.assign(netprofile::MAX_STORED_BYTES + 1, 0x30);
  EXPECT_EQ(0u, Store::entriesNeeded(big));
  FakeNvs nvs;
  Store store(nvs);
  EXPECT_NE(Error::None, store.install(big));
  EXPECT_TRUE(nvs.blobs.empty());
}

// ---- the card inbox ---------------------------------------------------------------------------------------------

namespace {

class FakeCard final : public netprofile::CardFiles {
 public:
  bool present = true;
  bool hasInbox = true;
  std::string inbox;
  bool wiped = false;
  std::string result;
  bool failWipe = false;

  bool available() override { return present; }
  int64_t inboxSize() override { return hasInbox ? static_cast<int64_t>(inbox.size()) : -1; }
  bool readInbox(uint8_t* out, size_t size) override {
    if (!hasInbox || size != inbox.size()) return false;
    std::copy(inbox.begin(), inbox.end(), out);
    return true;
  }
  bool wipeInbox() override {
    if (failWipe) return false;
    inbox.assign(inbox.size(), '\0');
    hasInbox = false;
    wiped = true;
    return true;
  }
  bool writeResult(std::string_view json) override {
    result = std::string(json);
    return true;
  }
};

}  // namespace

TEST(NetworkCardImport, AnOpenBundleIsInstalledAndTheInboxIsWiped) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard card;
  card.inbox = openBundle().text();
  const auto result = netprofile::importFromCard(card, store, options(), 1792000000);
  EXPECT_EQ(netprofile::CardOutcome::Installed, result.outcome);
  EXPECT_TRUE(result.wiped);
  EXPECT_TRUE(card.wiped);
  EXPECT_TRUE(result.resultWritten);
  EXPECT_NE(std::string::npos, card.result.find("installed"));
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::Ok, store.read(out));
}

TEST(NetworkCardImport, ABundleWithAPrivateKeyIsRefusedButStillWiped) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(pskBundle())));
  FakeCard card;
  card.inbox = eap().text();
  const auto result = netprofile::importFromCard(card, store, options(), 0);
  EXPECT_EQ(netprofile::CardOutcome::Rejected, result.outcome);
  EXPECT_EQ(Error::CardKeyRefused, result.error);
  EXPECT_TRUE(card.wiped);
  EXPECT_EQ(std::string::npos, card.result.find("PRIVATE KEY"));
  EXPECT_NE(std::string::npos, card.result.find("card_key_refused"));
  EXPECT_NE(std::string::npos, card.result.find("\"at\":null"));
  netprofile::Profile out;  // the working profile is untouched
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ(netprofile::Auth::Psk, out.info.auth);
}

TEST(NetworkCardImport, AnUnreadableBundleIsWipedWhateverTheResult) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard card;
  card.inbox = "this is not json";
  const auto result = netprofile::importFromCard(card, store, options(), 0);
  EXPECT_EQ(netprofile::CardOutcome::Rejected, result.outcome);
  EXPECT_EQ(Error::BadJson, result.error);
  EXPECT_TRUE(card.wiped);
  EXPECT_TRUE(result.resultWritten);
}

TEST(NetworkCardImport, ThePskIsNeverInTheResultFile) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard card;
  card.inbox = pskBundle().text();
  netprofile::importFromCard(card, store, options(), 1792000000);
  EXPECT_EQ(std::string::npos, card.result.find(testbundle::PSK));
  EXPECT_NE(std::string::npos, card.result.find("MSetup"));
}

TEST(NetworkCardImport, NoFileAndNoCard) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard none;
  none.hasInbox = false;
  EXPECT_EQ(netprofile::CardOutcome::NoFile, netprofile::importFromCard(none, store, options(), 0).outcome);
  EXPECT_FALSE(none.wiped);
  FakeCard gone;
  gone.present = false;
  gone.inbox = openBundle().text();
  EXPECT_EQ(netprofile::CardOutcome::NoCard, netprofile::importFromCard(gone, store, options(), 0).outcome);
  EXPECT_FALSE(gone.wiped);  // nothing is touched while the card is not the reader's
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::None, store.read(out));
}

TEST(NetworkCardImport, AWipeThatFailsIsReported) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard card;
  card.inbox = openBundle().text();
  card.failWipe = true;
  const auto result = netprofile::importFromCard(card, store, options(), 0);
  EXPECT_FALSE(result.wiped);
}

// ---- Import from card asks before it installs
// --------------------------------------------------------------------------

TEST(NetworkCardImport, ReadingTheCardInstallsNothingAndWipesTheInbox) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard card;
  card.inbox = pskBundle().text();
  card.result =
      "{\"schema\":1,\"at\":1790000000,\"result\":\"installed\",\"error\":null,\"profile\":null}\n";  // an earlier
                                                                                                      // import
  netprofile::CardRead read = netprofile::readCard(card, options(), 0);
  EXPECT_TRUE(read.pending);
  EXPECT_EQ(Error::None, read.error);
  EXPECT_TRUE(read.wiped);
  EXPECT_TRUE(card.wiped);  // the file is gone before the person has answered
  // The earlier import's result no longer stands while the question is up: the file says pending, with no secret in it.
  EXPECT_TRUE(read.resultWritten);
  EXPECT_NE(std::string::npos, card.result.find("\"result\":\"pending\""));
  EXPECT_NE(std::string::npos, card.result.find("\"schema\":1"));
  EXPECT_NE(std::string::npos, card.result.find("\"error\":null"));
  EXPECT_EQ(std::string::npos, card.result.find("installed"));
  EXPECT_EQ(std::string::npos, card.result.find(testbundle::PSK));
  EXPECT_NE(std::string::npos, card.result.find("MSetup"));
  EXPECT_EQ("MSetup", read.profile.info.ssid);
  EXPECT_EQ(netprofile::Auth::Psk, read.profile.info.auth);
  netprofile::Profile out;
  EXPECT_EQ(Store::Read::None, store.read(out));  // and nothing stored
}

TEST(NetworkCardImport, AcceptingInstallsAndWritesTheResult) {
  FakeNvs nvs;
  Store store(nvs);
  FakeCard card;
  card.inbox = pskBundle().text();
  netprofile::CardRead read = netprofile::readCard(card, options(), 0);
  ASSERT_TRUE(read.pending);
  const auto result = netprofile::finishCard(card, store, read, true, 1792000000);
  EXPECT_EQ(netprofile::CardOutcome::Installed, result.outcome);
  EXPECT_EQ(Error::None, result.error);
  EXPECT_TRUE(result.wiped);
  EXPECT_TRUE(result.resultWritten);
  EXPECT_NE(std::string::npos, card.result.find("\"result\":\"installed\""));
  EXPECT_EQ(std::string::npos, card.result.find(testbundle::PSK));
  EXPECT_FALSE(read.pending);
  netprofile::Profile out;
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ(netprofile::Auth::Psk, out.info.auth);
}

TEST(NetworkCardImport, DecliningStoresNothingAndSaysSoInTheResult) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None,
            store.install(make(openBundle().set("name", q("Working Profile")))));  // what Back must not touch
  FakeCard card;
  card.inbox = pskBundle().text();
  netprofile::CardRead read = netprofile::readCard(card, options(), 0);
  ASSERT_TRUE(read.pending);
  const auto result = netprofile::finishCard(card, store, read, false, 1792000000);
  EXPECT_EQ(netprofile::CardOutcome::Rejected, result.outcome);
  EXPECT_EQ(Error::Declined, result.error);
  EXPECT_TRUE(result.resultWritten);
  EXPECT_NE(std::string::npos, card.result.find("\"result\":\"rejected\""));
  EXPECT_NE(std::string::npos, card.result.find("\"error\":\"declined\""));
  EXPECT_EQ(std::string::npos, card.result.find(testbundle::PSK));
  netprofile::Profile out;
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ("Working Profile", out.info.name);  // the installed profile is as it was
  EXPECT_EQ(netprofile::Auth::Open, out.info.auth);
}

TEST(NetworkCardImport, ARefusedBundleIsFinalAtTheFirstStep) {
  FakeNvs nvs;
  FakeCard card;
  card.inbox = eap().text();
  const netprofile::CardRead read = netprofile::readCard(card, options(), 0);
  EXPECT_FALSE(read.pending);
  EXPECT_EQ(netprofile::CardOutcome::Rejected, read.outcome);
  EXPECT_EQ(Error::CardKeyRefused, read.error);
  EXPECT_TRUE(read.resultWritten);
  EXPECT_TRUE(card.wiped);
}

TEST(NetworkCardImport, AStorageFailureAfterAcceptingIsReportedAndTheOldProfileStays) {
  FakeNvs nvs;
  Store store(nvs);
  ASSERT_EQ(Error::None, store.install(make(openBundle().set("name", q("Working Profile")))));
  FakeCard card;
  card.inbox = pskBundle().text();
  netprofile::CardRead read = netprofile::readCard(card, options(), 0);
  ASSERT_TRUE(read.pending);
  nvs.available = 5;  // no room for another slot
  const auto result = netprofile::finishCard(card, store, read, true, 0);
  EXPECT_EQ(netprofile::CardOutcome::Rejected, result.outcome);
  EXPECT_EQ(Error::StorageFull, result.error);
  netprofile::Profile out;
  ASSERT_EQ(Store::Read::Ok, store.read(out));
  EXPECT_EQ("Working Profile", out.info.name);
}
