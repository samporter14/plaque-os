// The reader's own join code, run for real against a fake radio (see GlueStubs.cpp): the Method shelf's DeviceWifi, the
// connection check's DeviceNetwork, the profile join and the wake join in NetworkJoinDevice, and the installed profile
// in NVS behind them. The pure tests (NetworkJoinTest and the others) prove the decisions; these prove that the classes
// that carry them out are wired to them. A profile is installed through the same validate() and install() the reader
// uses, from synthetic bundles.
#include <TrustedTime.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "FakeWorld.h"
#include "NetworkText.h"
#include "TestBundles.h"
#include "activities/ambient/MethodDevice.h"
#include "activities/settings/ConnectionCheckDevice.h"
#include "network/CertificateDates.h"
#include "network/ClockProvenance.h"
#include "network/MessageTls.h"
#include "network/NetworkJoinDevice.h"
#include "network/NetworkProfileDevice.h"

namespace {

using fake::world;

class Glue : public ::testing::Test {
 protected:
  void SetUp() override {
    // lib/TrustedTime is the real one here and keeps its state in statics: bring it back to a first boot (then forget
    // what that did to the world).
    trustedtime::setFloorPolicy(nullptr);
    trustedtime::setSyncPrepareHook(nullptr);
    trustedtime::setServers(nullptr, 0);
    trustedtime::setUseDhcpServers(false);
    fake::reset();
    trustedtime::resetFloor();
    fake::reset();
    trustedtime::init();   // registers SNTP's notification callback, as setup() does
    clockprov::begin();    // forgets the sync record an earlier test made, and installs the floor policy
    netprofile::remove();  // forgets any profile an earlier test left in the cached summary
    trustedtime::setSyncPrepareHook(&netprofile::applyTimeSettings);  // as main() does
    // message_tls keeps its permission in statics: withdraw it, then start the world's clock from scratch.
    message_tls::beginClockSync();
    message_tls::abortClockSync();
    message_tls::withdrawClock();  // nor does a sync an earlier test made still count as this boot's
    world().clock = fake::Clock();
    netjoin::rememberJoined(netjoin::Joined::Nobody);  // RTC memory of a wake that never happened
    world().log.clear();
    world().errorLogs = 0;
  }

  void saved(const std::string& ssid, const std::string& password = std::string()) {
    world().store.networks.push_back({ssid, password});
  }

  void install(const testbundle::Bundle& bundle) {
    netprofile::Profile profile;
    ASSERT_EQ(netprofile::Error::None, netprofile::validate(bundle.text(), netprofile::Source::Usb, profile));
    ASSERT_EQ(netprofile::Error::None, netprofile::install(profile));
    ASSERT_TRUE(netprofile::installed());
  }

  void installOpen(const std::string& ssid = "MSetup") {
    install(testbundle::openBundle().set("ssid", testbundle::q(ssid)));
  }

  void installEap() {
    install(testbundle::eapBundle(testbundle::pki().certRsa, testpki::keyPem(testbundle::pki().clientRsa)));
  }

  static fake::AccessPoint openAp(const std::string& ssid, const int rssi = -50) {
    fake::AccessPoint ap;
    ap.ssid = ssid;
    ap.rssi = rssi;
    return ap;
  }

  static fake::AccessPoint pskAp(const std::string& ssid, const std::string& psk, const int rssi = -50) {
    fake::AccessPoint ap = openAp(ssid, rssi);
    ap.auth = WIFI_AUTH_WPA2_PSK;
    ap.psk = psk;
    return ap;
  }

  static fake::AccessPoint eapAp(const std::string& ssid, const int rssi = -50) {
    fake::AccessPoint ap = openAp(ssid, rssi);
    ap.auth = WIFI_AUTH_WPA2_ENTERPRISE;
    ap.radiusDomain = "radius.example.test";
    return ap;
  }
};

// Polls a join the way the screens do, once per 50 ms main-loop pass, until it ends or `limitMs` is spent.
template <typename Poll>
void pollUntilDone(Poll&& poll, const uint32_t limitMs = 40000) {
  for (uint32_t waited = 0; waited < limitMs; waited += 50) {
    if (poll()) return;
    fake::advance(50);
  }
}

}  // namespace

// ---- B1: Check connection joins the installed profile ---------------------------------------------------------------

TEST_F(Glue, CheckConnectionJoinsTheInstalledProfile) {
  saved("Home", "home-password");
  saved("Cafe", "cafe-password");
  installOpen("MSetup");
  world().radio.aps = {openAp("MSetup")};

  conncheck::DeviceNetwork network;
  const conncheck::NetworkInfo info = network.describe();
  EXPECT_TRUE(info.configured);
  EXPECT_EQ("profile", info.source);
  EXPECT_EQ("MSetup", info.ssid);

  network.beginJoin();
  conncheck::JoinState state = conncheck::JoinState::Pending;
  pollUntilDone([&] { return (state = network.joinState()) != conncheck::JoinState::Pending; });

  EXPECT_EQ(conncheck::JoinState::Joined, state);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("MSetup", world().radio.begins[0].ssid);
  // The profile is numbered after the saved networks: asking the saved-network store for it would be out of range.
  EXPECT_EQ(0, world().store.outOfRangeReads);
  EXPECT_EQ(0, fake::logCount("No usable network profile"));
}

TEST_F(Glue, CheckConnectionJoinsAPskProfile) {
  saved("Home", "home-password");
  install(testbundle::pskBundle());
  world().radio.aps = {pskAp("MSetup", testbundle::PSK)};

  conncheck::DeviceNetwork network;
  ASSERT_TRUE(network.describe().configured);
  network.beginJoin();
  conncheck::JoinState state = conncheck::JoinState::Pending;
  pollUntilDone([&] { return (state = network.joinState()) != conncheck::JoinState::Pending; });
  EXPECT_EQ(conncheck::JoinState::Joined, state);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_TRUE(world().radio.begins[0].hadPassphrase);
  EXPECT_EQ(testbundle::PSK, world().radio.begins[0].passphrase);
}

TEST_F(Glue, CheckConnectionJoinsAnEapTlsProfile) {
  saved("Home", "home-password");
  installEap();
  world().radio.aps = {eapAp("MWireless")};

  conncheck::DeviceNetwork network;
  ASSERT_TRUE(network.describe().configured);
  network.beginJoin();
  conncheck::JoinState state = conncheck::JoinState::Pending;
  pollUntilDone([&] { return (state = network.joinState()) != conncheck::JoinState::Pending; });
  EXPECT_EQ(conncheck::JoinState::Joined, state);
  EXPECT_TRUE(world().eap.enterpriseEnabled);
  EXPECT_EQ("radius.example.test", world().eap.domain);
  EXPECT_EQ(0, world().store.outOfRangeReads);
}

TEST_F(Glue, CheckConnectionWithoutAProfileStillUsesTheLastSavedNetwork) {
  saved("Home", "home-password");
  saved("Cafe", "cafe-password");
  world().store.lastConnected = "Cafe";
  world().radio.aps = {pskAp("Cafe", "cafe-password")};

  conncheck::DeviceNetwork network;
  const conncheck::NetworkInfo info = network.describe();
  EXPECT_EQ("saved", info.source);
  EXPECT_EQ("Cafe", info.ssid);
  network.beginJoin();
  conncheck::JoinState state = conncheck::JoinState::Pending;
  pollUntilDone([&] { return (state = network.joinState()) != conncheck::JoinState::Pending; });
  EXPECT_EQ(conncheck::JoinState::Joined, state);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("Cafe", world().radio.begins[0].ssid);
}

// ---- the Method shelf's own use of DeviceWifi
// -------------------------------------------------------------------------

TEST_F(Glue, TheShelfScansFirstWithAProfileAndJoinsItWhereItIsInRange) {
  saved("Home", "home-password");
  saved("Cafe", "cafe-password");
  world().store.lastConnected = "Cafe";
  installOpen("MSetup");
  world().radio.aps = {openAp("MSetup", -70), pskAp("Cafe", "cafe-password", -40), pskAp("Home", "home-password", -80)};

  method::DeviceWifi wifi;
  EXPECT_EQ(3u, wifi.savedCount());  // two saved networks and the profile
  EXPECT_EQ(-1, wifi.lastUsed());    // with a profile the shelf scans first
  wifi.beginScan();
  method::ScanStatus scan = method::ScanStatus::Pending;
  pollUntilDone([&] { return (scan = wifi.scanStatus()) != method::ScanStatus::Pending; });
  ASSERT_EQ(method::ScanStatus::Done, scan);
  // The profile first (it was seen), then the last-used network, then the other saved network.
  EXPECT_EQ((std::vector<size_t>{2, 1, 0}), wifi.visibleSaved());

  wifi.beginConnect(wifi.visibleSaved().front());
  method::WifiStatus status = method::WifiStatus::Pending;
  pollUntilDone([&] { return (status = wifi.connectStatus()) != method::WifiStatus::Pending; });
  EXPECT_EQ(method::WifiStatus::Connected, status);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("MSetup", world().radio.begins[0].ssid);
  EXPECT_EQ(0, world().store.outOfRangeReads);
}

TEST_F(Glue, AWakeJoinWithoutAProfileIsTheLastUsedNetworkAlone) {
  saved("Home", "home-password");
  saved("Cafe", "cafe-password");
  world().store.lastConnected = "Home";
  world().radio.aps = {pskAp("Home", "home-password")};
  EXPECT_TRUE(netjoin::joinForWake());
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("Home", world().radio.begins[0].ssid);
}

// ---- S1: the shelf's clock has the retained-clock fallback
// --------------------------------------------------------------

TEST_F(Glue, TheShelfClockOffersTheRetainedClockOfMessageTls) {
  constexpr int64_t synced = 1791028800;
  // A sync recorded ten minutes ago, since when the reader has kept time: the retained clock vouches.
  world().clock.timeEpoch = synced;
  world().clock.rtcMicros = 5000000;
  clockprov::recordSync();
  world().clock.rtcMicros += 600ull * 1000000;
  method::DeviceClock clock;
  EXPECT_FALSE(clock.clockReady());
  EXPECT_TRUE(clock.useRetained());
  EXPECT_TRUE(clock.clockReady());
}

TEST_F(Glue, TheShelfClockHasNoRetainedClockWithoutASyncRecord) {
  world().clock.timeEpoch = 1791028800;  // the NVS floor after a reset is not a clock anyone vouched for
  method::DeviceClock clock;
  EXPECT_FALSE(clock.useRetained());
  EXPECT_FALSE(clock.clockReady());
}

// ---- S2: the clock record judges every sync, and the retained clock rests on the proven floor
// ---------------------------------

namespace {
constexpr int64_t T0 = 1791028800;  // 2026-10-03T12:00:00Z
constexpr uint64_t RTC0 = 5000000;
constexpr uint64_t HOUR_US = 3600ull * 1000000;
constexpr int64_t DAY = 86400;

bool recordInNvs(clockprov::SyncRecord& out) {
  const auto& space = world().nvs.data["cptime"];
  const auto found = space.find("sync");
  if (found == space.end() || found->second.size() != sizeof(out)) return false;
  std::memcpy(&out, found->second.data(), sizeof(out));
  return true;
}

// The floor TrustedTime keeps in NVS (0 when it holds none).
int64_t persistedFloor() {
  const auto& space = world().nvs.data["cptime"];
  const auto found = space.find("floor");
  if (found == space.end() || found->second.size() != sizeof(int64_t)) return 0;
  int64_t value = 0;
  std::memcpy(&value, found->second.data(), sizeof(value));
  return value;
}

// A sync as message_tls makes it: the system clock was just set to `epoch`, `rtcMicros` into the RTC timeline.
clockprov::Verdict syncAt(const int64_t epoch, const uint64_t rtcMicros) {
  world().clock.timeEpoch = epoch;
  world().clock.rtcMicros = rtcMicros;
  return clockprov::recordSync();
}

// A reader whose chip time is confirmed: the first sync set the chip, the second (an hour later) agreed with it.
void establish() {
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0 + 3600, RTC0 + HOUR_US));
}

// What the person sees on the next boot: TrustedTime reads its floor, the clock record reads its record.
void reboot() {
  trustedtime::init();
  clockprov::begin();
}
}  // namespace

TEST_F(Glue, AnOrdinarySyncIsRecordedAndVouchesForTheTimeAfterwards) {
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  clockprov::SyncRecord record;
  ASSERT_TRUE(recordInNvs(record));
  EXPECT_EQ(T0, record.epoch);
  EXPECT_EQ(0, record.confirmed);                               // nothing but the network said so yet
  EXPECT_EQ((std::vector<int64_t>{T0}), world().chip.written);  // the RTC chip is set from the same second

  // An hour later the internal clock vouches for it.
  world().clock.rtcMicros = RTC0 + HOUR_US;
  world().clock.timeEpoch = T0 + 3600;
  const clockprov::Estimate estimate = clockprov::retained(0);
  EXPECT_TRUE(estimate.trusted);
  EXPECT_EQ(T0 + 3600, estimate.nowEpoch);
}

TEST_F(Glue, TheChipAgreeingWithTheNextSyncConfirmsItAndTheFloorFollows) {
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  EXPECT_EQ(0, trustedtime::provenFloor());  // the first sync had nothing to be witnessed by, and no floor to step from
  EXPECT_EQ(0, persistedFloor());
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0 + 3600 + 3, RTC0 + HOUR_US));  // the chip says T0 + 3600; 3 s apart
  clockprov::SyncRecord record;
  ASSERT_TRUE(recordInNvs(record));
  EXPECT_EQ(1, record.confirmed);
  EXPECT_EQ(T0 + 3600 + 3, record.epoch);
  EXPECT_EQ(T0 + 3600 + 3, trustedtime::provenFloor());
  EXPECT_EQ(T0 + 3600 + 3, persistedFloor());
}

TEST_F(Glue, ABackwardSpoofedSyncIsNeverRecordedNorTrusted) {
  // The reader has seen T0; a spoofed SNTP reply then set the system clock back by a year.
  trustedtime::raiseProvenFloor(T0);
  ASSERT_EQ(clockprov::Verdict::Refuse, syncAt(T0 - 365 * DAY, RTC0));
  clockprov::SyncRecord record;
  EXPECT_FALSE(recordInNvs(record));
  EXPECT_TRUE(world().chip.written.empty());  // nor is the RTC chip set from it
  EXPECT_GE(fake::logCount("not recorded"), 1);
  EXPECT_EQ(1, world().errorLogs);
  EXPECT_FALSE(clockprov::retained(0).trusted);

  // The same from a reader that already holds a good record: the old record stands, aged.
  world().errorLogs = 0;
  world().chip = fake::Chip();
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  ASSERT_TRUE(recordInNvs(record));
  EXPECT_EQ(T0, record.epoch);
  EXPECT_EQ(clockprov::Verdict::Refuse, syncAt(T0 - 180 * DAY, RTC0 + 600ull * 1000000));
  clockprov::SyncRecord after;
  ASSERT_TRUE(recordInNvs(after));
  EXPECT_EQ(T0, after.epoch);  // untouched
  EXPECT_EQ(1u, world().chip.written.size());
}

TEST_F(Glue, ASyncJustBehindTheFloorIsRecordedAsTheClockSetItNeverAheadOfIt) {
  // The floor is 72 s past the second this sync set the clock to. The chip is written with the sync's own second, not
  // the floor's: written ahead of the sync, its reading would carry an offset that the 2 s bound does not.
  trustedtime::raiseProvenFloor(T0 + 72);
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  clockprov::SyncRecord record;
  ASSERT_TRUE(recordInNvs(record));
  EXPECT_EQ(T0, record.epoch);
  EXPECT_EQ((std::vector<int64_t>{T0}), world().chip.written);
  // What is read back is raised to the floor, inside its bound.
  world().clock.rtcMicros = RTC0 + 1000000;
  const clockprov::Estimate estimate = clockprov::retained(0);
  EXPECT_TRUE(estimate.trusted);
  EXPECT_EQ(T0 + 72, estimate.nowEpoch);
}

TEST_F(Glue, TheRetainedTimeIsNeverBehindTheFloor) {
  world().chip.writable = false;  // no RTC chip set from the sync: the internal timer is what vouches
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  // An hour on, the proven floor is 40 s ahead of what the internal timer counted.
  world().clock.rtcMicros = RTC0 + HOUR_US;
  trustedtime::raiseProvenFloor(T0 + 3600 + 40);
  clockprov::Estimate estimate = clockprov::retained(0);
  EXPECT_TRUE(estimate.trusted);
  EXPECT_EQ(T0 + 3600 + 40, estimate.nowEpoch);
  // A floor ahead of the estimate by more than the tolerance (10 minutes) means the estimate is wrong: not trusted.
  trustedtime::raiseProvenFloor(T0 + 3600 + clockprov::FLOOR_TOLERANCE_S + 1);
  EXPECT_FALSE(clockprov::retained(0).trusted);
}

TEST_F(Glue, TheCrystalChipIsHeldToTheFloorToo) {
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));  // the chip is set from this sync
  ASSERT_FALSE(world().chip.written.empty());
  world().clock.rtcMicros = RTC0 + HOUR_US;  // an hour on: chip bound 2 s + 0.18 s
  trustedtime::raiseProvenFloor(T0 + 3600 + 2);
  clockprov::Estimate estimate = clockprov::retained(0);
  EXPECT_TRUE(estimate.trusted);
  EXPECT_EQ(T0 + 3600 + 2, estimate.nowEpoch);
  // Past the tolerance both the chip and the internal timer are ruled out.
  trustedtime::raiseProvenFloor(T0 + 3600 + clockprov::FLOOR_TOLERANCE_S + 1);
  EXPECT_FALSE(clockprov::retained(0).trusted);
}

TEST_F(Glue, TheExactChipIsNotVetoedByASystemClockThatRanFastThroughTheSleep) {
  // Synced at T0, slept a day. The reader's internal clock ran 2 % fast: the system clock (and so the monotonic floor,
  // which counts it) is 28 minutes 48 s ahead. The chip counted on its crystal and is exact; the floor that holds the
  // retained clock to account is what the syncs and the chip proved, not the system clock.
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));
  world().clock.rtcMicros = RTC0 + 24 * HOUR_US;
  world().clock.timeEpoch = T0 + DAY + DAY / 50;
  EXPECT_EQ(T0 + DAY + DAY / 50, trustedtime::trustedNow());  // the monotonic floor does include it
  const clockprov::Estimate estimate = clockprov::retained(0);
  EXPECT_TRUE(estimate.trusted);
  EXPECT_EQ(clockprov::Oscillator::CrystalRtc, estimate.oscillator);
  EXPECT_EQ(T0 + DAY, estimate.nowEpoch);
}

TEST_F(Glue, SleepEntryPersistsWhatTheChipVouchesForNotTheSystemClock) {
  establish();
  // Two days on, the system clock 2 % fast. note() is what the sleep path calls.
  world().clock.rtcMicros = RTC0 + HOUR_US + 48 * HOUR_US;
  world().clock.timeEpoch = T0 + 3600 + 2 * DAY + 2 * DAY / 50;
  trustedtime::note();
  const int64_t chipNow = T0 + 3600 + 2 * DAY;
  // Less the chip's own bound (2 s + 2 days x 50 ppm = 10.6 s, whole seconds 11): the floor cannot pass true time.
  EXPECT_EQ(chipNow - 11, trustedtime::provenFloor());
  EXPECT_EQ(chipNow - 11, persistedFloor());
  EXPECT_LT(persistedFloor(), world().clock.timeEpoch);
}

TEST_F(Glue, TheFloorNeverPersistsAChipTimeNobodyConfirmed) {
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0));  // the first sync: unconfirmed
  world().clock.rtcMicros = RTC0 + 48 * HOUR_US;
  trustedtime::note();
  EXPECT_EQ(0, persistedFloor());
  EXPECT_EQ(0, trustedtime::provenFloor());
}

TEST_F(Glue, ABackwardSetChipIsStillRejected) {
  establish();
  world().clock.rtcMicros = RTC0 + HOUR_US + 48 * HOUR_US;
  trustedtime::note();  // the sleep entry before the chip is touched: the floor is what the chip vouched for
  EXPECT_TRUE(clockprov::retained(0).trusted);
  // Someone sets the chip back an hour: behind the floor by more than the tolerance.
  world().chip.epoch -= 3600;
  EXPECT_FALSE(clockprov::retained(0).trusted);
  // Five minutes back is inside the tolerance: raised to the floor, not trusted at the chip's time.
  world().chip.epoch += 3600 - 300;
  const clockprov::Estimate near = clockprov::retained(0);
  EXPECT_TRUE(near.trusted);
  EXPECT_EQ(trustedtime::provenFloor(), near.nowEpoch);
  // And a day back is not the tolerance's business either.
  world().chip.epoch -= DAY;
  EXPECT_FALSE(clockprov::retained(0).trusted);
}

// ---- forward spoofs: what a reply the chip contradicts, and the first reply, can and cannot do
// ----------------------------------

namespace {
// What message_tls does for a caller that wants a clock: it asks SNTP, which answers `epoch`.
bool ntpAnswers(const int64_t epoch, const uint64_t rtcMicros) {
  world().clock.ntpEpoch = epoch;
  world().clock.rtcMicros = rtcMicros;
  return message_tls::prepareClock();
}
}  // namespace

TEST_F(Glue, AForwardSpoofTheChipContradictsIsUsedForTheSessionAndRecordedNowhere) {
  establish();
  clockprov::SyncRecord before;
  ASSERT_TRUE(recordInNvs(before));
  const size_t chipWrites = world().chip.written.size();
  const int64_t floorBefore = persistedFloor();
  for (const int64_t spoof : {DAY, 90 * DAY}) {
    world().errorLogs = 0;
    EXPECT_TRUE(ntpAnswers(T0 + 3600 + 600 + spoof, RTC0 + HOUR_US + 600ull * 1000000));
    clockprov::SyncRecord after;
    ASSERT_TRUE(recordInNvs(after));
    EXPECT_TRUE(clockprov::sameRecord(before, after)) << spoof;  // the record stands
    EXPECT_EQ(chipWrites, world().chip.written.size());          // the chip is not touched
    EXPECT_EQ(floorBefore, persistedFloor());                    // and the floor did not follow the lie
    EXPECT_EQ(floorBefore, trustedtime::provenFloor());
    EXPECT_GE(fake::logCount("this session only"), 1);
    EXPECT_EQ(1, world().errorLogs);  // at the level the serial log keeps
    // The lie does not outlive the boot: the next one starts from a floor that is not in the future.
    reboot();
    EXPECT_EQ(floorBefore, trustedtime::provenFloor());
    // The reader's retained clock is still the chip's.
    world().clock.timeEpoch = T0 + 3600 + 600;
    const clockprov::Estimate retained = clockprov::retained(0);
    EXPECT_TRUE(retained.trusted);
    EXPECT_EQ(clockprov::Oscillator::CrystalRtc, retained.oscillator);
    world().log.clear();
  }
  // An honest sync afterwards is recorded and moves the floor on.
  EXPECT_TRUE(ntpAnswers(T0 + 3600 + 600, RTC0 + HOUR_US + 600ull * 1000000));
  clockprov::SyncRecord honest;
  ASSERT_TRUE(recordInNvs(honest));
  EXPECT_EQ(T0 + 3600 + 600, honest.epoch);
  EXPECT_EQ(1, honest.confirmed);
  EXPECT_EQ(T0 + 3600 + 600, persistedFloor());
}

TEST_F(Glue, AnAnswerBeyondTheYear2100IsNotASyncAtAll) {
  establish();
  clockprov::SyncRecord before;
  ASSERT_TRUE(recordInNvs(before));
  const int64_t floorBefore = persistedFloor();
  world().errorLogs = 0;
  // The answer is not a sync: SNTP is not what vouches for the time (the chip's retained clock still does).
  EXPECT_TRUE(ntpAnswers(message_tls::MAX_TLS_EPOCH + 5 * 365 * DAY, RTC0 + HOUR_US + 600ull * 1000000));
  EXPECT_EQ(message_tls::ClockSource::Retained, message_tls::clockSource());
  clockprov::SyncRecord after;
  ASSERT_TRUE(recordInNvs(after));
  EXPECT_TRUE(clockprov::sameRecord(before, after));
  EXPECT_EQ(floorBefore, persistedFloor());
  EXPECT_EQ(floorBefore, trustedtime::provenFloor());
  EXPECT_FALSE(trustedtime::trustedNow() >= message_tls::MAX_TLS_EPOCH);  // never read back as a time
  // And note() does not keep it either.
  trustedtime::note();
  EXPECT_LT(persistedFloor(), message_tls::MAX_TLS_EPOCH);
}

TEST_F(Glue, AForwardSpoofOnTheVeryFirstSyncMovesTheFloorByAMinuteAtMost) {
  // A reader that has a floor already (say, from before the chip was set) and whose first recorded sync is a lie.
  trustedtime::raiseProvenFloor(T0);
  for (const int64_t spoof : {DAY, 90 * DAY}) {
    const int64_t floorBefore = persistedFloor();
    world().chip = fake::Chip();
    world().nvs.data["cptime"].erase("sync");
    reboot();
    EXPECT_TRUE(ntpAnswers(T0 + 120 + spoof, RTC0));
    clockprov::SyncRecord record;
    ASSERT_TRUE(recordInNvs(record));
    EXPECT_EQ(T0 + 120 + spoof, record.epoch);  // recorded: the chip needs something to witness the next sync with
    EXPECT_EQ(0, record.confirmed);
    EXPECT_EQ(floorBefore + clockprov::UNWITNESSED_STEP_S, persistedFloor());  // a minute, not the lie
    EXPECT_EQ(persistedFloor(), trustedtime::provenFloor());
    // The reboot after it starts from a floor that is nowhere near the lie, so an honest sync is not behind it.
    reboot();
    EXPECT_TRUE(ntpAnswers(T0 + 3600, RTC0 + HOUR_US));
    clockprov::SyncRecord healed;
    ASSERT_TRUE(recordInNvs(healed));
    EXPECT_EQ(T0 + 3600,
              healed.epoch);  // the honest sync replaced the lie: the chip disagreed, nobody had confirmed it
    EXPECT_EQ(T0 + 3600, world().chip.written.back());
  }
}

TEST_F(Glue, AReaderWithNoFloorPersistsNothingFromAnUnwitnessedSync) {
  EXPECT_TRUE(ntpAnswers(T0 + 90 * DAY, RTC0));
  EXPECT_EQ(0, persistedFloor());
  EXPECT_EQ(0, trustedtime::provenFloor());
  reboot();
  EXPECT_EQ(0, trustedtime::provenFloor());
}

TEST_F(Glue, AnAnswerFromTheDhcpLeaseServerIsJudgedLikeAnyOther) {
  establish();
  clockprov::SyncRecord before;
  ASSERT_TRUE(recordInNvs(before));
  install(testbundle::openBundle()
              .set("time_servers", "[\"ntp1.campus.test\", \"10.9.8.7\"]")
              .set("use_dhcp_time", "true"));
  netprofile::applyDhcpTimeSetting();       // what the join does before it starts
  fake::deliverLeaseNtpServer(0x0a000035);  // the lease names a time server
  EXPECT_EQ(0x0a000035u, world().clock.slotAddress[0]);
  world().errorLogs = 0;
  // The lease server answers 90 days ahead.
  EXPECT_TRUE(ntpAnswers(T0 + 3600 + 600 + 90 * DAY, RTC0 + HOUR_US + 600ull * 1000000));
  EXPECT_EQ(0x0a000035u, world().clock.slotAddress[0]);  // asked first, then the profile's own
  EXPECT_EQ((std::vector<std::string>{"ntp1.campus.test", "10.9.8.7"}), world().clock.serversSet.back());
  clockprov::SyncRecord after;
  ASSERT_TRUE(recordInNvs(after));
  EXPECT_TRUE(clockprov::sameRecord(before, after));
  EXPECT_EQ(before.epoch, persistedFloor());
  EXPECT_GE(fake::logCount("this session only"), 1);
}

TEST_F(Glue, ALeaseServerIsIgnoredWhenTheProfileDoesNotAcceptIt) {
  install(testbundle::openBundle().set("time_servers", "[\"ntp1.campus.test\"]"));
  netprofile::applyDhcpTimeSetting();
  fake::deliverLeaseNtpServer(0x0a000035);
  EXPECT_EQ(0u, world().clock.slotAddress[0]);
  EXPECT_TRUE(ntpAnswers(T0, RTC0));
  EXPECT_EQ((std::vector<std::string>{"ntp1.campus.test"}), world().clock.serversSet.back());
}

TEST_F(Glue, ResettingTheClockCheckClearsTheFloorAndTheRecordSoAReaderThatCannotConnectCan) {
  // A floor in the future, as an earlier firmware could have persisted from one spoofed reply.
  world().nvs.data["cptime"]["floor"] = std::vector<uint8_t>(sizeof(int64_t));
  const int64_t future = T0 + 90 * DAY;
  std::memcpy(world().nvs.data["cptime"]["floor"].data(), &future, sizeof(future));
  reboot();
  EXPECT_EQ(future, trustedtime::provenFloor());
  // The honest sync that follows is behind it: not recorded, and the retained clock has nothing.
  world().errorLogs = 0;
  EXPECT_TRUE(ntpAnswers(T0, RTC0));  // the session itself is not stopped
  clockprov::SyncRecord record;
  EXPECT_FALSE(recordInNvs(record));
  EXPECT_GE(fake::logCount("behind the clock floor"), 1);

  world().errorLogs = 0;
  clockprov::resetClockCheck();
  EXPECT_EQ(0, trustedtime::provenFloor());
  EXPECT_EQ(0, persistedFloor());
  EXPECT_FALSE(recordInNvs(record));
  EXPECT_GE(fake::logCount("clock check was reset"), 1);
  EXPECT_EQ(1, world().errorLogs);
  EXPECT_FALSE(clockprov::retained(0).trusted);
  // The next honest sync starts the chain again.
  EXPECT_TRUE(ntpAnswers(T0, RTC0));
  ASSERT_TRUE(recordInNvs(record));
  EXPECT_EQ(T0, record.epoch);
  EXPECT_EQ(0, record.confirmed);
}

TEST_F(Glue, ResettingTheClockCheckForgetsAConfirmedRecordAndTheSystemClockInTheFuture) {
  establish();
  world().clock.timeEpoch = T0 + 90 * DAY;  // this boot's system clock was set far ahead
  EXPECT_GE(trustedtime::trustedNow(), T0 + 90 * DAY);
  clockprov::resetClockCheck();
  clockprov::SyncRecord record;
  EXPECT_FALSE(recordInNvs(record));
  EXPECT_LT(world().clock.timeEpoch, T0);  // put back where no time is believed
  EXPECT_LT(trustedtime::trustedNow(), T0);
  // The chip still holds a time, but with no record nothing vouches for it.
  EXPECT_FALSE(clockprov::retained(0).trusted);
}

// ---- S3: the profile's time settings reach every sync, and the DHCP option precedes the join
// ------------------------------

namespace {
const std::vector<std::string> CAMPUS_SERVERS = {"ntp1.campus.test", "10.9.8.7"};
const std::vector<std::string> DEFAULT_SERVERS = {"pool.ntp.org", "time.google.com", "time.cloudflare.com"};
}  // namespace

TEST_F(Glue, ACallerOfPrepareClockGetsTheProfilesTimeServers) {
  install(testbundle::openBundle()
              .set("time_servers", "[\"ntp1.campus.test\", \"10.9.8.7\"]")
              .set("use_dhcp_time", "true"));
  world().clock.ntpEpoch = 1791028800;
  world().clock.trail.clear();
  world().clock.serversSet.clear();

  ASSERT_TRUE(message_tls::prepareClock());  // what the Notes poll, Register and the like call
  ASSERT_FALSE(world().clock.serversSet.empty());
  EXPECT_EQ(CAMPUS_SERVERS, world().clock.serversSet.back());
  ASSERT_FALSE(world().clock.dhcpFlagSet.empty());
  EXPECT_TRUE(world().clock.dhcpFlagSet.back());
  // ... and before the sync that uses them, not after.
  const auto& trail = world().clock.trail;
  const auto option = std::find(trail.begin(), trail.end(), "dhcpOn");
  const auto sync = std::find(trail.begin(), trail.end(), "configTzTime");
  ASSERT_NE(trail.end(), option);
  ASSERT_NE(trail.end(), sync);
  EXPECT_LT(option, sync);
}

TEST_F(Glue, TheShelfsClockSyncGetsThemToo) {
  install(testbundle::openBundle().set("time_servers", "[\"ntp1.campus.test\", \"10.9.8.7\"]"));
  world().clock.serversSet.clear();
  method::DeviceClock clock;
  clock.beginSync();
  ASSERT_FALSE(world().clock.serversSet.empty());
  EXPECT_EQ(CAMPUS_SERVERS, world().clock.serversSet.back());
}

TEST_F(Glue, EveryCallerOfASyncGetsTheProfilesServersBecauseTrustedTimeAsksForThem) {
  // main.cpp's wake path and the Wi-Fi setup call startSync() / syncNow() / beginSync() themselves, not through
  // message_tls: none of them has applied the profile's servers. TrustedTime's prepare hook does it for all.
  install(testbundle::openBundle().set("time_servers", "[\"ntp1.campus.test\", \"10.9.8.7\"]"));
  world().clock.ntpEpoch = 1791028800;

  world().clock.serversSet.clear();
  trustedtime::startSync();
  ASSERT_EQ(1u, world().clock.serversSet.size());
  EXPECT_EQ(CAMPUS_SERVERS, world().clock.serversSet.back());

  world().clock.sntpEnabled = false;
  world().clock.serversSet.clear();
  EXPECT_TRUE(trustedtime::syncNow(1000));
  ASSERT_EQ(1u, world().clock.serversSet.size());
  EXPECT_EQ(CAMPUS_SERVERS, world().clock.serversSet.back());

  world().clock.serversSet.clear();
  const uint32_t token = trustedtime::beginSync();
  ASSERT_EQ(1u, world().clock.serversSet.size());
  EXPECT_EQ(CAMPUS_SERVERS, world().clock.serversSet.back());
  fake::advance(10);
  EXPECT_TRUE(trustedtime::syncCompleted(token));

  // And the profile removed, the next sync asks the defaults again.
  netprofile::remove();
  world().clock.serversSet.clear();
  EXPECT_TRUE(trustedtime::syncNow(1000));
  EXPECT_EQ(DEFAULT_SERVERS, world().clock.serversSet.back());
}

TEST_F(Glue, WithoutAProfileASyncUsesTheDefaultServersAndNoDhcpOption) {
  world().clock.ntpEpoch = 1791028800;
  world().clock.serversSet.clear();
  world().clock.dhcpFlagSet.clear();
  ASSERT_TRUE(message_tls::prepareClock());
  ASSERT_FALSE(world().clock.serversSet.empty());
  EXPECT_EQ(DEFAULT_SERVERS, world().clock.serversSet.back());
  EXPECT_FALSE(world().clock.dhcpMode);
  for (const bool flag : world().clock.dhcpFlagSet) EXPECT_FALSE(flag);
}

TEST_F(Glue, TheDhcpTimeOptionIsOnBeforeTheProfileJoinStarts) {
  install(testbundle::openBundle().set("use_dhcp_time", "true"));
  world().radio.aps = {openAp("MSetup")};
  world().clock.trail.clear();
  ASSERT_TRUE(netjoin::beginProfileJoin());
  netjoin::Poll poll = netjoin::Poll::Pending;
  pollUntilDone([&] { return (poll = netjoin::pollProfileJoin()) != netjoin::Poll::Pending; });
  ASSERT_EQ(netjoin::Poll::Connected, poll);
  const auto& trail = world().clock.trail;
  const auto option = std::find(trail.begin(), trail.end(), "dhcpOn");
  const auto begin = std::find(trail.begin(), trail.end(), "wifiBegin");
  ASSERT_NE(trail.end(), option);
  ASSERT_NE(trail.end(), begin);
  EXPECT_LT(option, begin);
}

TEST_F(Glue, TheDhcpTimeOptionIsOnBeforeAWakeJoinStarts) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  install(testbundle::openBundle().set("use_dhcp_time", "true"));
  world().radio.aps = {openAp("MSetup")};
  world().clock.trail.clear();
  ASSERT_TRUE(netjoin::joinForWake());
  const auto& trail = world().clock.trail;
  const auto option = std::find(trail.begin(), trail.end(), "dhcpOn");
  const auto begin = std::find(trail.begin(), trail.end(), "wifiBegin");
  ASSERT_NE(trail.end(), option);
  ASSERT_NE(trail.end(), begin);
  EXPECT_LT(option, begin);
}

TEST_F(Glue, ANoProfileWakeJoinLeavesTheDhcpOptionOff) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  world().radio.aps = {pskAp("Home", "home-password")};
  ASSERT_TRUE(netjoin::joinForWake());
  for (const bool flag : world().clock.dhcpFlagSet) EXPECT_FALSE(flag);
}

// ---- S6: the sleep-event delivery joins the way a timer wake does
// -------------------------------------------------------

TEST_F(Glue, ASleepEventHasSomethingToJoinWithWhenThereIsAProfileOrALastUsedNetwork) {
  EXPECT_FALSE(netjoin::hasWakeCandidate());  // nothing saved, no profile
  saved("Home", "home-password");
  EXPECT_FALSE(netjoin::hasWakeCandidate());  // saved, but none was joined last: a wake never scans
  world().store.lastConnected = "Home";
  EXPECT_TRUE(netjoin::hasWakeCandidate());
  world().store.networks.clear();
  world().store.lastConnected.clear();
  installOpen();
  EXPECT_TRUE(netjoin::hasWakeCandidate());  // a profile alone is enough
}

TEST_F(Glue, ASleepEventDeliveryOnACertificateProfileJoinsItAndLeavesNoSupplicantStateBehind) {
  saved("Home", "home-password");  // joined last on some other day; the profile comes first
  world().store.lastConnected = "Home";
  installEap();
  world().radio.aps = {eapAp("MWireless")};
  ASSERT_TRUE(netjoin::hasWakeCandidate());
  ASSERT_TRUE(ambient::connectSavedWifi());
  EXPECT_TRUE(netjoin::joinedViaProfile());
  EXPECT_TRUE(world().eap.enterpriseEnabled);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("MWireless", world().radio.begins[0].ssid);

  ambient::disconnectWifi();  // what the sleep path now ends with
  EXPECT_FALSE(world().eap.enterpriseEnabled);
  EXPECT_FALSE(world().eap.identitySet);
  EXPECT_FALSE(world().eap.caSet);
  EXPECT_FALSE(world().eap.certSet);
  EXPECT_FALSE(world().eap.keySet);
  EXPECT_EQ(WIFI_MODE_NULL, WiFi.getMode());
}

// ---- S4: a wake's join remembers who joined last, and gives each candidate a budget
// ------------------------------------

namespace {
uint32_t joinForWakeTime() {
  const uint32_t started = static_cast<uint32_t>(millis());
  netjoin::joinForWake();
  return static_cast<uint32_t>(millis()) - started;
}

fake::AccessPoint slow(fake::AccessPoint ap, const uint32_t joinMs) {
  ap.joinMs = joinMs;
  return ap;
}
}  // namespace

TEST_F(Glue, AWakeJoinTriesTheProfileFirstWhenNoneJoinedLast) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installOpen();
  world().radio.aps = {openAp("MSetup"), pskAp("Home", "home-password")};
  ASSERT_TRUE(netjoin::joinForWake());
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("MSetup", world().radio.begins[0].ssid);
  EXPECT_EQ(netjoin::Joined::Profile, netjoin::lastJoined());
}

TEST_F(Glue, AWakeJoinTriesTheSavedNetworkFirstWhenItJoinedLast) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installOpen();
  world().radio.aps = {openAp("MSetup"), pskAp("Home", "home-password")};
  netjoin::rememberJoined(netjoin::Joined::Saved);
  ASSERT_TRUE(netjoin::joinForWake());
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("Home", world().radio.begins[0].ssid);
  EXPECT_EQ(netjoin::Joined::Saved, netjoin::lastJoined());
}

TEST_F(Glue, AWakeAtHomeThenTriesHomeFirstTheNextTime) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installOpen();
  world().radio.aps = {pskAp("Home", "home-password")};  // the campus network is out of range
  ASSERT_TRUE(netjoin::joinForWake());
  ASSERT_EQ(2u, world().radio.begins.size());  // the profile first, nobody having joined; it is not here
  EXPECT_EQ("MSetup", world().radio.begins[0].ssid);
  EXPECT_EQ("Home", world().radio.begins[1].ssid);
  EXPECT_EQ(netjoin::Joined::Saved, netjoin::lastJoined());

  ambient::disconnectWifi();
  world().radio.begins.clear();
  ASSERT_TRUE(netjoin::joinForWake());
  ASSERT_EQ(1u, world().radio.begins.size());  // now Home first, and Home joins: the profile is never tried
  EXPECT_EQ("Home", world().radio.begins[0].ssid);
}

TEST_F(Glue, ACertificateProfileHasTimeToFinishAWakeJoin) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installEap();
  world().radio.aps = {slow(eapAp("MWireless"), 18000)};  // beyond the 10 s the whole join used to have
  const uint32_t took = joinForWakeTime();
  EXPECT_TRUE(netjoin::joinedViaProfile());
  EXPECT_GE(took, 18000u);
  EXPECT_LE(took, 18100u);
}

TEST_F(Glue, AWakeJoinThatHasToFallBackStaysInsideItsBudget) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installEap();
  // The certificate network hangs (never finishes); the saved network is in range.
  world().radio.aps = {slow(eapAp("MWireless"), 100000), pskAp("Home", "home-password")};
  const uint32_t took = joinForWakeTime();
  EXPECT_EQ(WL_CONNECTED, WiFi.status());
  ASSERT_EQ(2u, world().radio.begins.size());
  EXPECT_EQ("MWireless", world().radio.begins[0].ssid);
  EXPECT_EQ("Home", world().radio.begins[1].ssid);
  EXPECT_GE(took, 25000u);  // the profile had all of its 25 s
  EXPECT_LE(took, netjoin::wakeJoinWorstMs(true, true));
  EXPECT_EQ(netjoin::Joined::Saved, netjoin::lastJoined());
}

TEST_F(Glue, ASavedNetworkThatHangsDoesNotCostTheProfileItsBudget) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installEap();
  netjoin::rememberJoined(netjoin::Joined::Saved);
  world().radio.aps = {slow(pskAp("Home", "home-password"), 100000), slow(eapAp("MWireless"), 15000)};
  const uint32_t took = joinForWakeTime();
  EXPECT_TRUE(netjoin::joinedViaProfile());
  ASSERT_EQ(2u, world().radio.begins.size());
  EXPECT_EQ("Home", world().radio.begins[0].ssid);
  EXPECT_EQ("MWireless", world().radio.begins[1].ssid);
  EXPECT_GE(took, 10000u + 15000u);  // 10 s for Home, then the profile's own 15 s
  EXPECT_LE(took, 25200u);
  EXPECT_EQ(netjoin::Joined::Profile, netjoin::lastJoined());
}

TEST_F(Glue, AWakeJoinWithoutAProfileIsStillTenSecondsForTheLastUsedNetwork) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  world().radio.aps = {slow(pskAp("Home", "home-password"), 11000)};
  const uint32_t tooSlow = joinForWakeTime();
  EXPECT_NE(WL_CONNECTED, WiFi.status());
  EXPECT_GE(tooSlow, 10000u);
  EXPECT_LE(tooSlow, 10100u);

  ambient::disconnectWifi();
  world().radio.aps = {slow(pskAp("Home", "home-password"), 9000)};
  EXPECT_TRUE(netjoin::joinForWake());
  EXPECT_EQ(netjoin::Joined::Saved, netjoin::lastJoined());
}

TEST_F(Glue, ABackToBackManualJoinRemembersWhoJoined) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installOpen();
  world().radio.aps = {openAp("MSetup"), pskAp("Home", "home-password")};
  method::DeviceWifi wifi;
  ASSERT_EQ(2u, wifi.savedCount());  // one saved network and the profile
  wifi.noteJoined(1);                // the profile (numbered after the one saved network)
  EXPECT_EQ(netjoin::Joined::Profile, netjoin::lastJoined());
  wifi.noteJoined(0);  // the saved network
  EXPECT_EQ(netjoin::Joined::Saved, netjoin::lastJoined());
}

// ---- the EAP-TLS join: only an enterprise network, and the supplicant put back as it was
// ------------------------------

TEST_F(Glue, AnEapTlsJoinNeverAssociatesWithAnOpenOrPskNetworkOfTheSameName) {
  installEap();
  // The open and the PSK networks are stronger; the enterprise one is the only one that can be right.
  world().radio.aps = {openAp("MWireless", -30), pskAp("MWireless", "whatever", -35), eapAp("MWireless", -60)};
  ASSERT_TRUE(netjoin::beginProfileJoin());
  netjoin::Poll poll = netjoin::Poll::Pending;
  pollUntilDone([&] { return (poll = netjoin::pollProfileJoin()) != netjoin::Poll::Pending; });
  EXPECT_EQ(netjoin::Poll::Connected, poll);
  ASSERT_EQ(1u, world().radio.attempts.size());
  EXPECT_EQ(WIFI_AUTH_WPA2_ENTERPRISE, world().radio.attempts[0].threshold);
  EXPECT_TRUE(world().radio.attempts[0].enterpriseEnabled);
  ASSERT_GE(world().radio.attempts[0].target, 0);
  EXPECT_EQ(WIFI_AUTH_WPA2_ENTERPRISE, world().radio.aps[static_cast<size_t>(world().radio.attempts[0].target)].auth);
  EXPECT_EQ(WIFI_AUTH_WPA2_ENTERPRISE, world().radio.joinedAuth);
  EXPECT_EQ(1, world().radio.connectCalls);  // begun without connecting, then connected once, with the threshold set
}

TEST_F(Glue, AnEapTlsJoinWithOnlyAnOpenTwinInRangeDoesNotJoinAtAll) {
  installEap();
  world().radio.aps = {openAp("MWireless", -30)};
  ASSERT_TRUE(netjoin::beginProfileJoin());
  netjoin::Poll poll = netjoin::Poll::Pending;
  pollUntilDone([&] { return (poll = netjoin::pollProfileJoin()) != netjoin::Poll::Pending; });
  EXPECT_EQ(netjoin::Poll::Failed, poll);
  EXPECT_NE(WL_CONNECTED, WiFi.status());
  EXPECT_TRUE(world().radio.joinedSsid.empty());  // never associated, not even to be turned away afterwards
  ASSERT_EQ(1u, world().radio.attempts.size());
  EXPECT_EQ(-1, world().radio.attempts[0].target);  // no acceptable access point: the open one was never a candidate
  EXPECT_EQ(WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD, world().radio.failReason);
}

TEST_F(Glue, AnOpenOrPskProfileJoinIsAsItWas) {
  installOpen();
  world().radio.aps = {openAp("MSetup")};
  ASSERT_TRUE(netjoin::beginProfileJoin());
  netjoin::Poll poll = netjoin::Poll::Pending;
  pollUntilDone([&] { return (poll = netjoin::pollProfileJoin()) != netjoin::Poll::Pending; });
  EXPECT_EQ(netjoin::Poll::Connected, poll);
  EXPECT_EQ(WIFI_AUTH_OPEN, world().radio.attempts[0].threshold);
  EXPECT_FALSE(world().radio.attempts[0].enterpriseEnabled);
}

TEST_F(Glue, ReleasingTheEnterpriseStateRestoresTheSupplicantsDefaults) {
  installEap();
  world().radio.aps = {eapAp("MWireless")};
  ASSERT_TRUE(netjoin::beginProfileJoin());
  netjoin::Poll poll = netjoin::Poll::Pending;
  pollUntilDone([&] { return (poll = netjoin::pollProfileJoin()) != netjoin::Poll::Pending; });
  ASSERT_EQ(netjoin::Poll::Connected, poll);
  EXPECT_EQ(static_cast<uint32_t>(ESP_EAP_TYPE_TLS), world().eap.methods);  // EAP-TLS and nothing else while it is used

  netjoin::releaseEnterprise();
  EXPECT_EQ(static_cast<uint32_t>(ESP_EAP_TYPE_ALL), world().eap.methods);
  EXPECT_FALSE(world().eap.enterpriseEnabled);
  EXPECT_FALSE(world().eap.identitySet);
  EXPECT_TRUE(world().eap.domain.empty());
}

TEST_F(Glue, ACheckWithNoProfileDoesNotLogAnErrorForIt) {
  EXPECT_FALSE(netjoin::beginProfileJoin());
  EXPECT_EQ(0, world().errorLogs);
  EXPECT_EQ(0, fake::logCount("No usable network profile"));
}

// ---- the install question: what it says about what is installed, what is replaced, and what the reader cannot do
// ----------

namespace {
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
size_t lines(const std::string& text) { return 1 + static_cast<size_t>(std::count(text.begin(), text.end(), '\n')); }
}  // namespace

TEST_F(Glue, TheQuestionForACertificateProfileSaysWhatTheReaderCannotCheckAndHowTheKeyIsKept) {
  netprofile::Profile profile;
  ASSERT_EQ(netprofile::Error::None,
            netprofile::validate(
                testbundle::eapBundle(testbundle::pki().certRsa, testpki::keyPem(testbundle::pki().clientRsa)).text(),
                netprofile::Source::Usb, profile));
  const std::string details = netui::profileDetails(profile.info, true);
  EXPECT_TRUE(has(details, "Network name: MWireless"));
  EXPECT_TRUE(has(details, "Security: Certificate (EAP-TLS)"));
  EXPECT_TRUE(has(details, "Server name: radius.example.test"));
  EXPECT_TRUE(has(details, "The server certificate's dates aren't checked."));
  EXPECT_TRUE(has(details, "The key is stored unencrypted in the reader."));
  EXPECT_FALSE(has(details, "Replaces"));         // nothing installed yet
  EXPECT_FALSE(has(details, "clock isn't set"));  // the client certificate's dates were judged
  EXPECT_LE(lines(details), 9u);                  // e-ink space is tight: the dialog shows at most 14 lines
  // Without a trusted clock the client certificate's dates were not judged, and it says so, as before.
  EXPECT_TRUE(has(netui::profileDetails(profile.info, false), "clock isn't set"));
}

TEST_F(Glue, ThePasswordOfAPskProfileIsSaidToBeKeptUnencryptedToo) {
  netprofile::Profile profile;
  ASSERT_EQ(netprofile::Error::None,
            netprofile::validate(testbundle::pskBundle().text(), netprofile::Source::Usb, profile));
  const std::string details = netui::profileDetails(profile.info, false);
  EXPECT_TRUE(has(details, "The password is stored unencrypted in the reader."));
  EXPECT_FALSE(has(details, "server certificate"));
  netprofile::Profile open;
  ASSERT_EQ(netprofile::Error::None,
            netprofile::validate(testbundle::openBundle().text(), netprofile::Source::Usb, open));
  const std::string openDetails = netui::profileDetails(open.info, false);
  EXPECT_FALSE(has(openDetails, "unencrypted"));  // there is no secret in an open profile
}

TEST_F(Glue, TheQuestionSaysWhatItReplacesAndThatACertificateProfilesKeyIsDeleted) {
  installEap();
  const netprofile::Summary installed = netprofile::summary();
  netprofile::Profile incoming;
  ASSERT_EQ(netprofile::Error::None,
            netprofile::validate(testbundle::pskBundle().text(), netprofile::Source::Card, incoming));
  const std::string details = netui::profileDetails(incoming.info, false, &installed);
  EXPECT_TRUE(has(details, "Replaces Lab 100 Wi-Fi, Certificate (EAP-TLS). Its key is deleted from the reader."));
  EXPECT_LE(lines(details), 9u);

  // An open or PSK profile in the way has no key to lose.
  netprofile::remove();
  installOpen();
  const netprofile::Summary open = netprofile::summary();
  const std::string openDetails = netui::profileDetails(incoming.info, false, &open);
  EXPECT_TRUE(has(openDetails, "Replaces Lab 100 Wi-Fi, Open."));
  EXPECT_FALSE(has(openDetails, "deleted"));
  // And with nothing installed there is nothing to say.
  netprofile::remove();
  const netprofile::Summary none = netprofile::summary();
  EXPECT_FALSE(has(netui::profileDetails(incoming.info, false, &none), "Replaces"));
}

TEST_F(Glue, TheQuestionDrawsAnSsidWithoutItsInvisibleCharacters) {
  netprofile::Profile profile;
  ASSERT_EQ(netprofile::Error::None,
            netprofile::validate(testbundle::openBundle().set("ssid", testbundle::q("Lab\xE2\x80\xAEWiFi")).text(),
                                 netprofile::Source::Usb, profile));
  const std::string details = netui::profileDetails(profile.info, false);
  EXPECT_FALSE(has(details, "\xE2\x80\xAE"));
  EXPECT_TRUE(has(details,
                  "Network name: Lab\xEF\xBF\xBD"
                  "WiFi"));
  EXPECT_EQ("Lab\xE2\x80\xAEWiFi", profile.info.ssid);  // what is stored, and what the Mac sent, is untouched
}

// ---- Import from card through the reader's own card and NVS code
// ----------------------------------------------------------

namespace {
constexpr char INBOX[] = "/Lamina/Provision/network-profile.json";
constexpr char IMPORT_RESULT[] = "/Lamina/network-import.json";
}  // namespace

TEST_F(Glue, ABundleOnTheCardIsReadAndWipedButNotInstalledUntilTheAnswerIsYes) {
  world().card.files[INBOX] = testbundle::pskBundle().text();
  netprofile::CardRead read = netprofile::beginCardImport();
  ASSERT_TRUE(read.pending);
  EXPECT_EQ(0u, world().card.files.count(INBOX));  // the file is gone before the question is asked
  ASSERT_EQ(1u,
            world().card.files.count(IMPORT_RESULT));  // and the card says the import is pending, not what the last was
  EXPECT_TRUE(has(world().card.files[IMPORT_RESULT], "\"result\":\"pending\""));
  EXPECT_FALSE(netprofile::installed());

  const netprofile::CardResult result = netprofile::finishCardImport(read, true);
  EXPECT_EQ(netprofile::CardOutcome::Installed, result.outcome);
  EXPECT_TRUE(netprofile::installed());
  EXPECT_EQ(netprofile::Auth::Psk, netprofile::summary().auth);
  ASSERT_EQ(1u, world().card.files.count(IMPORT_RESULT));
  EXPECT_TRUE(has(world().card.files[IMPORT_RESULT], "\"result\":\"installed\""));
  EXPECT_FALSE(has(world().card.files[IMPORT_RESULT], testbundle::PSK));
}

TEST_F(Glue, BackOnTheQuestionLeavesTheInstalledProfileAndSaysDeclinedOnTheCard) {
  installEap();
  world().card.files[INBOX] = testbundle::pskBundle().text();
  netprofile::CardRead read = netprofile::beginCardImport();
  ASSERT_TRUE(read.pending);
  const netprofile::CardResult result = netprofile::finishCardImport(read, false);
  EXPECT_EQ(netprofile::CardOutcome::Rejected, result.outcome);
  EXPECT_EQ(netprofile::Error::Declined, result.error);
  EXPECT_TRUE(has(world().card.files[IMPORT_RESULT], "\"error\":\"declined\""));
  // The certificate profile, and the key behind it, are still installed and still join.
  EXPECT_EQ(netprofile::Auth::EapTls, netprofile::summary().auth);
  netprofile::Profile joinable;
  EXPECT_TRUE(netprofile::loadForJoin(joinable));
  EXPECT_FALSE(joinable.keyDer.empty());
}

TEST_F(Glue, ACardBundleWithAKeyIsRefusedAtOnceAndNothingIsAsked) {
  world().card.files[INBOX] =
      testbundle::eapBundle(testbundle::pki().certRsa, testpki::keyPem(testbundle::pki().clientRsa)).text();
  const netprofile::CardRead read = netprofile::beginCardImport();
  EXPECT_FALSE(read.pending);
  EXPECT_EQ(netprofile::Error::CardKeyRefused, read.error);
  EXPECT_EQ(0u, world().card.files.count(INBOX));
  EXPECT_FALSE(netprofile::installed());
}

// ---- a board that has no network profile has none
// -----------------------------------------------------------------------

namespace {
// A C3 X4, a Sticky, a Paper Mono: no Plaque feature at all (lib/PlaqueBoard).
void useAnUnsupportedBoard() {
  world().boardFacts = plaque::BoardFacts();
  world().boardFacts.touch = true;
}
}  // namespace

TEST_F(Glue, OnAnUnsupportedBoardAProfileLeftInNvsIsNotThere) {
  installOpen();  // as if an earlier firmware, or a flash from another board, had left one
  ASSERT_TRUE(netprofile::installed());
  useAnUnsupportedBoard();
  netprofile::reload();
  EXPECT_FALSE(netprofile::installed());
  EXPECT_FALSE(netprofile::summary().installed);
  EXPECT_TRUE(netprofile::activeTimeServers().empty());
  netprofile::Profile joinable;
  EXPECT_FALSE(netprofile::loadForJoin(joinable));
  EXPECT_FALSE(netjoin::hasWakeCandidate());  // a profile is not a reason to join
  EXPECT_FALSE(netjoin::beginProfileJoin());
  EXPECT_EQ(0, world().errorLogs);
}

TEST_F(Glue, OnAnUnsupportedBoardNothingCanBeInstalledFromUsbOrTheCard) {
  netprofile::Profile profile;
  ASSERT_EQ(netprofile::Error::None,
            netprofile::validate(testbundle::pskBundle().text(), netprofile::Source::Usb, profile));  // well formed
  useAnUnsupportedBoard();
  // Over USB the bundle is refused at validation, and a profile validated elsewhere is not installed either.
  netprofile::Profile again;
  EXPECT_EQ(netprofile::Error::AuthUnsupported,
            netprofile::validate(testbundle::pskBundle().text(), netprofile::Source::Usb, again));
  EXPECT_EQ(netprofile::Error::AuthUnsupported, netprofile::install(profile));
  EXPECT_FALSE(netprofile::installed());
  netprofile::reload();
  EXPECT_FALSE(netprofile::installed());
  EXPECT_LE(world().nvs.data["plaque_net"].size(), 1u);  // no slot was written (at most the "none active" pointer)

  world().card.files[INBOX] = testbundle::pskBundle().text();
  const netprofile::CardRead read = netprofile::beginCardImport();
  EXPECT_FALSE(read.pending);
  EXPECT_EQ(netprofile::CardOutcome::NoCard, read.outcome);
  EXPECT_EQ(1u, world().card.files.count(INBOX));  // the card is not looked at, let alone wiped
  EXPECT_EQ(0u, world().card.files.count(IMPORT_RESULT));
}

TEST_F(Glue, TheProfileSummaryFileIsWrittenOnlyWhereTheBoardHasProfiles) {
  installOpen();
  EXPECT_EQ(1u, world().card.files.count("/Lamina/network-profile.json"));  // the X4 Pro writes it
  world().card.files.erase("/Lamina/network-profile.json");
  useAnUnsupportedBoard();
  netprofile::refreshSummaryFile();
  EXPECT_EQ(0u, world().card.files.count("/Lamina/network-profile.json"));
  world().boardFacts = fake::proBoardFacts();
  netprofile::refreshSummaryFile();
  EXPECT_EQ(1u, world().card.files.count("/Lamina/network-profile.json"));
}

TEST_F(Glue, TheClassicHasEveryNetworkFeature) {
  world().boardFacts = plaque::BoardFacts();
  world().boardFacts.x4Classic = true;
  world().boardFacts.usbDrive = true;
  netprofile::reload();
  installOpen();
  EXPECT_TRUE(netprofile::installed());
  EXPECT_EQ(1u, world().card.files.count("/Lamina/network-profile.json"));
  world().card.files[INBOX] = testbundle::pskBundle().text();
  EXPECT_TRUE(netprofile::beginCardImport().pending);
}

// ---- a wake that tries the saved network first still turns the DHCP time option on first
// ------------------------------

TEST_F(Glue, ASavedNetworkFirstWakeStillSetsTheDhcpTimeOptionBeforeItsJoin) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  install(testbundle::openBundle().set("use_dhcp_time", "true"));
  netjoin::rememberJoined(netjoin::Joined::Saved);  // home joined last, so it goes first
  world().radio.aps = {pskAp("Home", "home-password"), openAp("MSetup")};
  world().clock.trail.clear();
  ASSERT_TRUE(netjoin::joinForWake());
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("Home", world().radio.begins[0].ssid);  // the saved network, not the profile
  const auto& trail = world().clock.trail;
  const auto option = std::find(trail.begin(), trail.end(), "dhcpOn");
  const auto begin = std::find(trail.begin(), trail.end(), "wifiBegin");
  ASSERT_NE(trail.end(), option);  // the option is the profile's, and the profile is installed whoever joins
  ASSERT_NE(trail.end(), begin);
  EXPECT_LT(option, begin);
}

// ---- Register: a poll over the network profile ---------------------------------------------------------------------
//
// The Register screen's scheduled poll is a timer wake like Notes': main.cpp joins with ambient::connectSavedWifi()
// (the profile-aware wake join) and RegisterProvider::fetch() starts with message_tls::prepareClock() for an https
// mailbox. RegisterProvider.cpp draws, so it is not compiled for the host; these run those two calls, in that order,
// against the fake radio, supplicant, SNTP client and system clock, and test/test_register_seams.py holds the order in
// the source.

namespace {
struct RegisterPollRun {
  bool joined = false;
  bool viaProfile = false;
  bool clockReady = false;
  uint32_t joinMs = 0;
};

// What a Register timer wake does with the network.
RegisterPollRun registerPollWake() {
  RegisterPollRun run;
  const uint32_t started = static_cast<uint32_t>(millis());
  run.joined = ambient::connectSavedWifi();
  run.joinMs = static_cast<uint32_t>(millis()) - started;
  if (!run.joined) return run;
  run.viaProfile = netjoin::joinedViaProfile();
  run.clockReady = message_tls::prepareClock();
  return run;
}

// What "Check the register now" does while it waits for the profile (RegisterActivity, State::JoiningProfile): poll
// until it connects, fails, or the screen's time is out, then give the profile up.
bool interactiveProfileJoin(uint32_t* tookMs = nullptr) {
  const bool eapTls = netprofile::summary().auth == netprofile::Auth::EapTls;
  if (!(netprofile::installed() && netjoin::beginProfileJoin())) return false;
  const uint32_t started = static_cast<uint32_t>(millis());
  const uint32_t deadline = started + netjoin::interactiveProfileJoinMs(eapTls);
  bool connected = false;
  for (;;) {
    const netjoin::Poll poll = netjoin::pollProfileJoin();
    if (poll == netjoin::Poll::Connected) {
      connected = true;
      break;
    }
    if (poll == netjoin::Poll::Failed || static_cast<int32_t>(static_cast<uint32_t>(millis()) - deadline) >= 0) {
      netjoin::abortProfileJoin();
      break;
    }
    fake::advance(50);
  }
  if (tookMs) *tookMs = static_cast<uint32_t>(millis()) - started;
  return connected;
}
}  // namespace

TEST_F(Glue, ARegisterPollOverACertificateProfileJoinsItAndSyncsTheClockWithItsServers) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  install(testbundle::eapBundle(testbundle::pki().certRsa, testpki::keyPem(testbundle::pki().clientRsa))
              .set("time_servers", "[\"ntp1.campus.test\", \"10.9.8.7\"]"));
  world().radio.aps = {eapAp("MWireless"), pskAp("Home", "home-password")};
  world().clock.ntpEpoch = 1791028800;
  world().clock.serversSet.clear();

  const RegisterPollRun run = registerPollWake();
  ASSERT_TRUE(run.joined);
  EXPECT_TRUE(run.viaProfile);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("MWireless", world().radio.begins[0].ssid);  // nobody joined last: the profile first
  EXPECT_LE(run.joinMs, netjoin::wakeJoinWorstMs(true, true));
  ASSERT_TRUE(run.clockReady);
  ASSERT_FALSE(world().clock.serversSet.empty());
  EXPECT_EQ(CAMPUS_SERVERS, world().clock.serversSet.back()) << "the Register poll gets the profile's time servers";

  ambient::disconnectWifi();  // the wake's end: the radio off and nothing of the supplicant left behind
  EXPECT_EQ(WIFI_MODE_NULL, WiFi.getMode());
  EXPECT_FALSE(world().eap.enterpriseEnabled);
  EXPECT_FALSE(world().eap.keySet);
  EXPECT_EQ(0, world().errorLogs);
}

TEST_F(Glue, ARegisterPollOnAReaderWhoseOnlyNetworkIsTheProfileCanRun) {
  installOpen("MSetup");  // no saved network at all
  world().radio.aps = {openAp("MSetup")};
  world().clock.ntpEpoch = 1791028800;
  ASSERT_TRUE(netjoin::hasWakeCandidate()) << "the wake has something to try";
  const RegisterPollRun run = registerPollWake();
  EXPECT_TRUE(run.joined && run.viaProfile && run.clockReady);
  EXPECT_EQ(netjoin::Joined::Profile, netjoin::lastJoined());
}

TEST_F(Glue, ARegisterPollAfterAWakeAtHomeTriesHomeFirstAndKeepsToItsBudget) {
  saved("Home", "home-password");
  world().store.lastConnected = "Home";
  installEap();
  netjoin::rememberJoined(netjoin::Joined::Saved);
  // The campus network hangs; Home is in range and goes first, so the poll never waits for the campus.
  world().radio.aps = {slow(eapAp("MWireless"), 100000), pskAp("Home", "home-password")};
  world().clock.ntpEpoch = 1791028800;
  const RegisterPollRun run = registerPollWake();
  ASSERT_TRUE(run.joined);
  EXPECT_FALSE(run.viaProfile);
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("Home", world().radio.begins[0].ssid);
  EXPECT_TRUE(run.clockReady);
}

TEST_F(Glue, ARegisterPollOnAProWithNoProfileIsTheOldTenSecondJoinAndTheDefaultServers) {
  // The Register branch alone: the last-used network, 10 s, no scan; the default time servers and no DHCP option.
  saved("Home", "home-password");
  saved("Cafe", "cafe-password");
  world().store.lastConnected = "Home";
  world().radio.aps = {pskAp("Cafe", "cafe-password"),
                       slow(pskAp("Home", "home-password"), 100000)};  // Home never joins
  world().clock.ntpEpoch = 1791028800;
  world().clock.serversSet.clear();
  world().clock.dhcpFlagSet.clear();
  const RegisterPollRun run = registerPollWake();
  EXPECT_FALSE(run.joined) << "no scan, no second network: the poll just does not happen";
  ASSERT_EQ(1u, world().radio.begins.size());
  EXPECT_EQ("Home", world().radio.begins[0].ssid);
  EXPECT_GE(run.joinMs, 10000u);
  EXPECT_LE(run.joinMs, 10100u);
  EXPECT_FALSE(netjoin::joinedViaProfile());
  ambient::disconnectWifi();

  world().radio.aps = {pskAp("Home", "home-password")};
  world().radio.begins.clear();
  const RegisterPollRun again = registerPollWake();
  ASSERT_TRUE(again.joined && again.clockReady);
  EXPECT_EQ(DEFAULT_SERVERS, world().clock.serversSet.back());
  for (const bool flag : world().clock.dhcpFlagSet) EXPECT_FALSE(flag);
}

TEST_F(Glue, CheckTheRegisterNowWaitsAsLongAsMessagesDoesForTheProfile) {
  EXPECT_EQ(20000u, netjoin::interactiveProfileJoinMs(false));  // the values MessageViewActivity had
  EXPECT_EQ(30000u, netjoin::interactiveProfileJoinMs(true));
  EXPECT_LT(netjoin::interactiveProfileJoinMs(false), 60000u);

  // An EAP-TLS network that takes 18 s is waited for ...
  installEap();
  world().radio.aps = {slow(eapAp("MWireless"), 18000)};
  uint32_t took = 0;
  EXPECT_TRUE(interactiveProfileJoin(&took));
  EXPECT_GE(took, 18000u);
  EXPECT_LT(took, 18200u);
  EXPECT_TRUE(netjoin::joinedViaProfile());
  ambient::disconnectWifi();

  // ... one that never finishes is given up on at 30 s, with the supplicant left clean for the saved networks.
  world().radio.aps = {slow(eapAp("MWireless"), 100000)};
  EXPECT_FALSE(interactiveProfileJoin(&took));
  EXPECT_GE(took, 30000u);
  EXPECT_LT(took, 30200u);
  EXPECT_FALSE(world().eap.enterpriseEnabled);
  EXPECT_FALSE(world().eap.keySet);
}

TEST_F(Glue, CheckTheRegisterNowGivesAnOpenProfileTwentySecondsAndThenTheSavedNetworksGetTheirTurn) {
  saved("Home", "home-password");
  installOpen("MSetup");
  world().radio.aps = {slow(openAp("MSetup"), 100000), pskAp("Home", "home-password")};
  uint32_t took = 0;
  EXPECT_FALSE(interactiveProfileJoin(&took));
  EXPECT_GE(took, 20000u);
  EXPECT_LT(took, 20200u);
  // The screen then hands over to the Wi-Fi screen (WifiSelectionActivity), which joins the saved network; the radio is
  // not left half way through the profile's attempt.
  EXPECT_NE(WL_CONNECTED, WiFi.status());
}

// ---- a floor an earlier firmware left ahead of true time, and the reset
// -----------------------------------------------
//
// 2a6befc persisted the raw internal clock at every sleep entry, and that clock runs fast. A reader that replaces it
// has a floor minutes or hours ahead of the truth and no clock record, so every honest sync was refused as behind the
// floor until real time passed it (the reviewer measured 2 refused syncs at 900 s ahead, 17 at an hour, 117 at six
// hours).

namespace {
void setPersistedFloor(const int64_t value) {
  auto& stored = world().nvs.data["cptime"]["floor"];
  stored = std::vector<uint8_t>(sizeof(value));
  std::memcpy(stored.data(), &value, sizeof(value));
}

// A power-on boot of a reader whose NVS holds a floor and no clock record: the system clock restarts at zero and
// TrustedTime restores the floor into it, as setup() does.
void coldBootWithFloor(const int64_t floor) {
  world().clock.resetReason = 1;
  world().clock.rtcMicros = RTC0;
  world().clock.timeEpoch = 0;
  setPersistedFloor(floor);
  world().nvs.data["cptime"].erase("sync");
  reboot();
}

// The chip as an earlier firmware's sync left it: answering, with the time `epoch` right now.
void chipReads(const int64_t epoch) {
  world().chip.readable = true;
  world().chip.epoch = epoch;
  world().chip.setAtRtcMicros = world().clock.rtcMicros;
}
}  // namespace

TEST_F(Glue, TheFirstSyncAfterAnOlderFirmwareLowersAFloorLeftAheadAndNoHonestSyncIsRefused) {
  for (const int64_t ahead : {900LL, 3600LL, 6 * 3600LL, DAY}) {
    SetUp();
    coldBootWithFloor(T0 + ahead);
    ASSERT_EQ(T0 + ahead, trustedtime::provenFloor());
    ASSERT_EQ(T0 + ahead, world().clock.timeEpoch) << "the cold boot resumes from the floor";
    int refused = 0;
    for (int i = 0; i < 6; ++i) {  // an honest sync every three minutes
      if (syncAt(T0 + 180 * i, RTC0 + 180ull * 1000000 * i) != clockprov::Verdict::Record) ++refused;
    }
    EXPECT_EQ(0, refused) << "floor " << ahead << " s ahead";
    EXPECT_EQ(T0 + 180 * 5, persistedFloor()) << "the floor follows the truth from the first sync on";
    EXPECT_EQ(persistedFloor(), trustedtime::provenFloor());
    clockprov::SyncRecord record;
    ASSERT_TRUE(recordInNvs(record));
    EXPECT_GE(fake::logCount("floor an earlier firmware left was ahead") + fake::logCount("lowered to"), 1);
  }
}

TEST_F(Glue, TheFloorComesDownToTheFirstSyncAndTrustedNowComesWithIt) {
  coldBootWithFloor(T0 + 3600);
  EXPECT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0 + 1000000));
  EXPECT_EQ(T0, persistedFloor());
  EXPECT_EQ(T0, trustedtime::provenFloor());
  EXPECT_EQ(T0, trustedtime::trustedNow()) << "not an hour ahead for the rest of the boot";
  clockprov::SyncRecord record;
  ASSERT_TRUE(recordInNvs(record));
  EXPECT_EQ(T0, record.epoch);
  EXPECT_EQ(0, record.confirmed);
  EXPECT_EQ((std::vector<int64_t>{T0}), world().chip.written) << "the chip is set from the sync";
}

TEST_F(Glue, ARecordedChipThatAgreesWitnessesTheLoweringAndOneThatSidesWithTheFloorDoesNot) {
  // The chip an earlier firmware set from an honest sync: it agrees, the floor comes down.
  coldBootWithFloor(T0 + 3600);
  world().clock.rtcMicros = RTC0 + 1000000;
  chipReads(T0 + 2);
  EXPECT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0 + 1000000));
  EXPECT_EQ(T0, persistedFloor());

  // A chip that reads the floor's time says the network went backward: refused, the floor stays.
  SetUp();
  coldBootWithFloor(T0 + 3600);
  world().clock.rtcMicros = RTC0 + 1000000;
  chipReads(T0 + 3600);
  EXPECT_EQ(clockprov::Verdict::Refuse, syncAt(T0, RTC0 + 1000000));
  EXPECT_EQ(T0 + 3600, persistedFloor());
  EXPECT_EQ(T0 + 3600, trustedtime::provenFloor());
  clockprov::SyncRecord record;
  EXPECT_FALSE(recordInNvs(record));
  EXPECT_GE(fake::logCount("behind the clock floor"), 1);

  // A floor more than a day ahead is not forgiven (the person resets the clock check).
  SetUp();
  coldBootWithFloor(T0 + DAY + 60);
  EXPECT_EQ(clockprov::Verdict::Refuse, syncAt(T0, RTC0 + 1000000));
  EXPECT_EQ(T0 + DAY + 60, persistedFloor());
}

TEST_F(Glue, OnceARecordExistsABackwardSyncIsStillRefusedAndTheFloorStays) {
  coldBootWithFloor(T0 + 3600);
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0 + 1000000));  // lowers the floor, once
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0 + 600, RTC0 + 601ull * 1000000));
  const int64_t floorNow = persistedFloor();
  ASSERT_EQ(T0 + 600, floorNow);
  // A router that answers an hour, a day, a week back: refused, and the floor, the record and the chip are left alone.
  for (const int64_t back : {3600LL, DAY, 7 * DAY}) {
    clockprov::SyncRecord before;
    ASSERT_TRUE(recordInNvs(before));
    const size_t chipWrites = world().chip.written.size();
    EXPECT_EQ(clockprov::Verdict::Refuse, syncAt(T0 + 600 - back, RTC0 + 700ull * 1000000)) << back;
    EXPECT_EQ(floorNow, persistedFloor());
    EXPECT_EQ(floorNow, trustedtime::provenFloor());
    clockprov::SyncRecord after;
    ASSERT_TRUE(recordInNvs(after));
    EXPECT_TRUE(clockprov::sameRecord(before, after));
    EXPECT_EQ(chipWrites, world().chip.written.size());
  }
}

TEST_F(Glue, ResettingTheClockCheckLeavesNoTimeAtAllAndTakesBackTheHttpsPermission) {
  world().clock.ntpEpoch = T0;
  ASSERT_TRUE(message_tls::prepareClock());
  EXPECT_TRUE(message_tls::clockReady());
  EXPECT_TRUE(message_tls::clockSyncedThisBoot());
  EXPECT_EQ(T0, trustedtime::trustedNow());

  clockprov::resetClockCheck();
  // The reader reads as having no time: not the earliest plausible second (which trustedNow() would believe), and the
  // 90 seconds the sync allowed are gone with the clock they were earned on.
  EXPECT_LT(world().clock.timeEpoch, trustedtime::MIN_VALID_EPOCH);
  EXPECT_EQ(0, trustedtime::trustedNow());
  EXPECT_FALSE(message_tls::clockReady());
  EXPECT_EQ(message_tls::ClockSource::None, message_tls::clockSource());
  EXPECT_FALSE(message_tls::clockSyncedThisBoot());
  EXPECT_FALSE(message_tls::certificateClockTrusted());
  // And it stays so: a second later the clock has not become a time.
  world().clock.timeEpoch += 5;
  EXPECT_EQ(0, trustedtime::trustedNow());
  // The next honest sync starts the chain again.
  EXPECT_TRUE(message_tls::prepareClock());
  EXPECT_EQ(T0, trustedtime::trustedNow());
  EXPECT_TRUE(message_tls::clockSyncedThisBoot());
}

TEST_F(Glue, ABootWithOnlyARestoredFloorHasNoSyncAndNoRetainedClockForRegister) {
  // After a flash or a power loss: the system clock is the floor, which is "not earlier than", not a time anyone
  // vouched for. RegisterProvider::registerNow() asks exactly these two and gives Register no time unless one is true.
  coldBootWithFloor(T0 + 600);
  EXPECT_NE(0, trustedtime::trustedNow());
  EXPECT_FALSE(message_tls::clockSyncedThisBoot());
  EXPECT_FALSE(clockprov::retained(0).trusted);
  // A sync this boot settles it ...
  ASSERT_EQ(clockprov::Verdict::Record, syncAt(T0, RTC0 + 1000000));
  world().clock.ntpEpoch = T0;
  ASSERT_TRUE(message_tls::prepareClock());
  EXPECT_TRUE(message_tls::clockSyncedThisBoot());
}

TEST_F(Glue, ADeepSleepWakeWithARecordedSyncHasARetainedClockForRegister) {
  establish();
  world().clock.resetReason = 3;  // a timer wake from deep sleep: RTC memory and the timer survived
  world().clock.rtcMicros += 3600ull * 1000000;
  world().clock.timeEpoch += 3600;
  reboot();
  EXPECT_FALSE(message_tls::clockSyncedThisBoot()) << "no sync this boot, as on a boundary wake";
  EXPECT_TRUE(clockprov::retained(0).trusted) << "but the clock has run since a recorded sync";
}
