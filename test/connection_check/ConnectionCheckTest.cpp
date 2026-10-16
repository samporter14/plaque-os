#include <ArduinoJson.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "network/ConnectionCheck.h"

using conncheck::AddressInfo;
using conncheck::AddressState;
using conncheck::Check;
using conncheck::Detail;
using conncheck::JoinState;
using conncheck::Message;
using conncheck::Outcome;
using conncheck::StageId;
using conncheck::Status;

namespace {
constexpr char DEVICE[] = "lab-reader";
constexpr char HOST[] = "mailbox.example.workers.dev";
constexpr char TOKEN[] = "tok-3f9a1c7e5b2d4f60";
constexpr char PSK[] = "correct horse battery staple";
constexpr char SECRET_TITLE[] = "Unpublished Assay Protocol";
constexpr uint32_t STEP_MS = 10;  // one main-loop pass

// ---- fakes ---------------------------------------------------------------------------------------------------------

struct FakeTime : conncheck::TimePort {
  uint32_t ms = 100000;
  // The sync: answers Synced (or Failed when syncFails) this long after beginSync(); never when negative.
  int64_t syncAfterMs = 1200;
  bool syncFails = false;
  bool retainedTrusted = false;
  bool guardHeld = false;  // the sync's power-save guard
  uint32_t syncStartedAt = 0;
  int beginCalls = 0;
  int retainedCalls = 0;

  uint32_t nowMs() override { return ms; }
  void beginSync() override {
    ++beginCalls;
    guardHeld = true;
    syncStartedAt = ms;
  }
  method::SyncStatus syncStatus() override {
    if (!guardHeld) return method::SyncStatus::Failed;
    if (syncAfterMs < 0 || ms - syncStartedAt < syncAfterMs) return method::SyncStatus::Pending;
    guardHeld = false;
    return syncFails ? method::SyncStatus::Failed : method::SyncStatus::Synced;
  }
  void abortSync() override { guardHeld = false; }
  bool useRetained() override {
    ++retainedCalls;
    return retainedTrusted;
  }
};

struct FakeNetwork : conncheck::NetworkPort {
  FakeTime& time;
  conncheck::NetworkInfo info;
  std::string password = PSK;  // what the reader joins with: must never reach the file
  int64_t joinAfterMs = 3000;  // associated this long after beginJoin(); never when negative
  int64_t failAfterMs = -1;    // the driver gives up this long after beginJoin()
  int reason = 0;              // reported from reasonAfterMs on
  int64_t reasonAfterMs = 0;
  int64_t addressAfterMs = 800;  // after the join; never when negative
  bool partialAddress = false;   // an address without a gateway
  int64_t linkDropAfterMs = -1;  // after the join
  int eapChecked = -1;
  bool radioOn = false;
  bool joining = false;
  uint32_t joinStartedAt = 0;
  uint32_t joinedAt = 0;
  bool joined = false;
  int beginCalls = 0;
  int offCalls = 0;

  explicit FakeNetwork(FakeTime& t) : time(t) {
    info.configured = true;
    info.source = "saved";
    info.name = "Lab 100";
    info.ssid = "Lab 100";
    info.auth = "psk";
  }
  uint32_t since(uint32_t at) const { return time.ms - at; }
  conncheck::NetworkInfo describe() override { return info; }
  void beginJoin() override {
    ++beginCalls;
    radioOn = true;
    joining = true;
    joined = false;
    joinStartedAt = time.ms;
  }
  JoinState joinState() override {
    if (!joining) return JoinState::Failed;
    if (joined) return JoinState::Joined;
    if (failAfterMs >= 0 && since(joinStartedAt) >= failAfterMs) return JoinState::Failed;
    if (joinAfterMs >= 0 && since(joinStartedAt) >= joinAfterMs) {
      joined = true;
      joinedAt = time.ms;
      return JoinState::Joined;
    }
    return JoinState::Pending;
  }
  int disconnectReason() override { return reason != 0 && since(joinStartedAt) >= reasonAfterMs ? reason : 0; }
  int rssi() override { return -61; }
  int channel() override { return 6; }
  int eapTimeChecked() override { return eapChecked; }
  bool linkUp() override { return joined && (linkDropAfterMs < 0 || since(joinedAt) < linkDropAfterMs); }
  AddressState address(AddressInfo& out) override {
    if (!joined || addressAfterMs < 0 || since(joinedAt) < addressAfterMs) return AddressState::None;
    std::snprintf(out.ip, sizeof(out.ip), "%s", "10.1.2.3");
    if (partialAddress) return AddressState::Partial;
    std::snprintf(out.gateway, sizeof(out.gateway), "%s", "10.1.0.1");
    std::snprintf(out.dns[0], sizeof(out.dns[0]), "%s", "10.0.0.53");
    std::snprintf(out.dns[1], sizeof(out.dns[1]), "%s", "10.0.0.54");
    out.dnsCount = 2;
    return AddressState::Complete;
  }
  void off() override {
    ++offCalls;
    radioOn = false;
    joining = false;
    joined = false;
  }
};

struct FakeHttp : conncheck::HttpPort {
  FakeTime& time;
  // The name lookup.
  int64_t resolveAfterMs = 150;  // never when negative
  bool resolveFails = false;
  bool lookupPending = false;
  uint32_t resolveStartedAt = 0;
  int resolveAborts = 0;
  // The heap check.
  int readyWaits = 0;  // negative: always Wait
  bool noMemory = false;
  // The request.
  method::HttpOpen openResult = method::HttpOpen::Ok;
  httpfail::Outcome failure;   // reported when openResult is not Ok
  uint32_t openTakesMs = 900;  // the blocking connect, handshake and headers
  std::string body;
  int64_t length = -2;  // -2: the body's size
  size_t slice = 700;
  size_t stallAt = SIZE_MAX;
  bool failMidway = false;
  bool connectionOpen = false;
  size_t offset = 0;
  int opens = 0;
  std::vector<std::string> paths;

  explicit FakeHttp(FakeTime& t) : time(t) {}
  void beginResolve() override {
    lookupPending = true;
    resolveStartedAt = time.ms;
  }
  method::ResolveStatus resolveStatus() override {
    if (!lookupPending) return method::ResolveStatus::Failed;
    if (resolveAfterMs < 0 || time.ms - resolveStartedAt < resolveAfterMs) return method::ResolveStatus::Pending;
    lookupPending = false;
    return resolveFails ? method::ResolveStatus::Failed : method::ResolveStatus::Done;
  }
  void abortResolve() override {
    if (lookupPending) ++resolveAborts;
    lookupPending = false;
  }
  method::HttpReady ready() override {
    if (readyWaits != 0) {
      if (readyWaits > 0) --readyWaits;
      return method::HttpReady::Wait;
    }
    return noMemory ? method::HttpReady::NoMemory : method::HttpReady::Yes;
  }
  method::HttpOpen open(const char* path, size_t) override {
    ++opens;
    paths.emplace_back(path);
    time.ms += openTakesMs;
    if (openResult != method::HttpOpen::Ok) return openResult;
    connectionOpen = true;
    offset = 0;
    return method::HttpOpen::Ok;
  }
  httpfail::Outcome outcome() override {
    if (openResult != method::HttpOpen::Ok) return failure;
    httpfail::Outcome ok;
    ok.status = 200;
    return ok;
  }
  int64_t contentLength() override { return length == -2 ? static_cast<int64_t>(body.size()) : length; }
  method::HttpRead read(uint8_t* buffer, size_t capacity, size_t& got) override {
    got = 0;
    if (!connectionOpen) return method::HttpRead::Failed;
    if (offset >= stallAt) return method::HttpRead::Idle;
    if (offset >= body.size()) return failMidway ? method::HttpRead::Failed : method::HttpRead::Done;
    if (failMidway && offset > 0) return method::HttpRead::Failed;
    got = std::min({capacity, slice, body.size() - offset, stallAt - offset});
    std::memcpy(buffer, body.data() + offset, got);
    offset += got;
    return method::HttpRead::Data;
  }
  void close() override { connectionOpen = false; }
};

struct Log : conncheck::Observer {
  std::vector<std::string> lines;
  void stageEnded(StageId stage, Status status, Detail detail, uint32_t ms) override {
    lines.push_back(std::string(conncheck::stageName(stage)) + " " + conncheck::statusName(status) + " " +
                    conncheck::detailName(detail) + " " + std::to_string(ms));
  }
};

std::string bookList(const char* title = "RNA extraction") {
  return std::string("{\"books\":[{\"book_id\":\"b12\",\"kind\":\"method\",\"title\":\"") + title + "\",\"sha256\":\"" +
         std::string(64, 'a') +
         "\",\"byte_length\":5000,\"page_count\":8,\"created_at\":1790000000,\"book_url\":\"/api/device/" + DEVICE +
         "/book/b12\"}]}";
}

httpfail::Outcome failureOf(httpfail::Failure kind, int status = 0, const char* location = "") {
  httpfail::Outcome outcome;
  outcome.failure = kind;
  outcome.status = status;
  std::snprintf(outcome.locationHost, sizeof(outcome.locationHost), "%s", location);
  return outcome;
}

struct Rig {
  FakeTime time;
  FakeNetwork network{time};
  FakeHttp http{time};
  Log log;
  conncheck::Config config;
  conncheck::Limits limits;
  std::unique_ptr<Check> check;
  int steps = 0;
  uint32_t enteredAt = 0;  // when runUntil()'s stage began

  Rig() {
    config.configured = true;
    config.deviceId = DEVICE;
    config.host = HOST;
    http.body = bookList(SECRET_TITLE);
  }

  void start() {
    check.reset();
    conncheck::Ports ports{network, time, http, &log};
    check = std::make_unique<Check>(ports, config, limits);
    check->start();
  }

  // Steps like the screen does: once per main-loop pass, 10 ms apart.
  void run(int maxSteps = 100000) {
    start();
    for (; steps < maxSteps && check->running(); ++steps) {
      check->step();
      time.ms += STEP_MS;
    }
  }

  // Steps until the check reaches `stage` (still running there). The clock is left where the stage began.
  void runUntil(StageId stage) {
    start();
    for (int i = 0; i < 100000 && check->running() && check->stage() != stage; ++i) {
      time.ms += STEP_MS;
      check->step();
    }
    enteredAt = time.ms;
    ASSERT_TRUE(check->running());
    ASSERT_EQ(check->stage(), stage);
  }

  const conncheck::Report& report() const { return check->report(); }
  Status status(StageId id) const { return report().stage(id).status; }
  Detail detail(StageId id) const { return report().stage(id).detail; }

  // Every way out leaves the radio off, the guards released, the connection closed and the lookup abandoned.
  void expectCleanedUp() const {
    EXPECT_FALSE(check->running());
    EXPECT_TRUE(check->finished());
    EXPECT_FALSE(network.radioOn) << "Wi-Fi left on";
    EXPECT_FALSE(network.joining) << "join left running";
    EXPECT_GE(network.offCalls, 1);
    EXPECT_FALSE(time.guardHeld) << "clock sync (and its power-save guard) left running";
    EXPECT_FALSE(http.connectionOpen) << "connection (and its power-save guard) left open";
    EXPECT_FALSE(http.lookupPending) << "name lookup left running";
  }

  void expectLaterNotRun(StageId failed) const {
    for (size_t i = static_cast<size_t>(failed) + 1; i < conncheck::STAGE_COUNT; ++i)
      EXPECT_EQ(report().stages[i].status, Status::NotRun) << conncheck::stageName(static_cast<StageId>(i));
  }

  void expectEarlierOk(StageId failed) const {
    for (size_t i = 0; i < static_cast<size_t>(failed); ++i)
      EXPECT_EQ(report().stages[i].status, Status::Ok) << conncheck::stageName(static_cast<StageId>(i));
  }

  void expectFailure(StageId stage, Detail detail, Message message) const {
    expectCleanedUp();
    EXPECT_EQ(report().outcome, Outcome::Failed);
    EXPECT_EQ(report().message, message);
    expectEarlierOk(stage);
    EXPECT_EQ(status(stage), Status::Failed);
    EXPECT_EQ(this->detail(stage), detail);
    expectLaterNotRun(stage);
  }

  std::string json(int64_t finishedAt = 1792000123) const {
    conncheck::ResultContext context;
    context.finishedAt = finishedAt;
    context.clock = finishedAt ? clockprov::Source::Sntp : clockprov::Source::None;
    context.bootMs = 48211;
    context.readerUuid = "0b6c9f9e-3c1a-4c7e-9f00-1d2e3f4a5b6c";
    context.model = "x4c";
    context.firmware = "1.6.5-x4c+2a6befc";
    context.stationMac = "AA:BB:CC:DD:EE:FF";
    context.mailboxHost = config.configured ? HOST : "";
    return conncheck::resultJson(context, report());
  }
};

JsonDocument parse(const std::string& text) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
  return doc;
}
}  // namespace

// ---- the whole way --------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, EveryStageSucceeds) {
  Rig rig;
  rig.run();
  rig.expectCleanedUp();
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
  EXPECT_EQ(rig.report().message, Message::Connected);
  for (const auto& stage : rig.report().stages) EXPECT_EQ(stage.status, Status::Ok);
  EXPECT_EQ(rig.report().timeSource, clockprov::Source::Sntp);
  EXPECT_EQ(rig.report().rssi, -61);
  EXPECT_EQ(rig.report().channel, 6);
  EXPECT_STREQ(rig.report().address.ip, "10.1.2.3");
  EXPECT_EQ(rig.report().httpStatus, 200);
  EXPECT_EQ(rig.http.opens, 1);
  ASSERT_EQ(rig.http.paths.size(), 1u);
  EXPECT_EQ(rig.http.paths[0], std::string("/api/device/") + DEVICE + "/books");
  EXPECT_EQ(rig.time.beginCalls, 1);
  EXPECT_EQ(rig.time.retainedCalls, 0);
  EXPECT_EQ(rig.log.lines.size(), 6u);
  EXPECT_EQ(rig.report().stage(StageId::Join).ms, 3000u);
}

TEST(ConnectionCheck, StagesRunInTheContractOrder) {
  Rig rig;
  std::vector<StageId> seen;
  rig.start();
  while (rig.check->running()) {
    if (seen.empty() || seen.back() != rig.check->stage()) seen.push_back(rig.check->stage());
    rig.check->step();
    rig.time.ms += STEP_MS;
  }
  const std::vector<StageId> expected = {StageId::Join, StageId::Network, StageId::Dns,
                                         StageId::Time, StageId::Https,   StageId::Mailbox};
  EXPECT_EQ(seen, expected);
}

TEST(ConnectionCheck, NotSetUpTouchesNothingAndRunsNoStage) {
  Rig rig;
  rig.config.configured = false;
  rig.run();
  rig.expectCleanedUp();
  EXPECT_EQ(rig.report().outcome, Outcome::NotSetUp);
  EXPECT_EQ(rig.report().message, Message::NotSetUp);
  for (const auto& stage : rig.report().stages) EXPECT_EQ(stage.status, Status::NotRun);
  EXPECT_EQ(rig.network.beginCalls, 0);
  EXPECT_EQ(rig.http.opens, 0);
  EXPECT_EQ(rig.time.beginCalls, 0);
  EXPECT_STREQ(conncheck::messageText(Message::NotSetUp),
               "The mailbox isn't set up. Add it under Settings \xE2\x80\xBA System \xE2\x80\xBA Messages.");
}

// ---- join -----------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, JoinNothingConfigured) {
  Rig rig;
  rig.network.info = conncheck::NetworkInfo{};
  rig.run();
  rig.expectFailure(StageId::Join, Detail::NothingConfigured, Message::JoinFailed);
  EXPECT_EQ(rig.network.beginCalls, 0);
}

TEST(ConnectionCheck, JoinNetworkNotSeen) {
  Rig rig;
  rig.network.joinAfterMs = -1;
  rig.network.reason = 201;
  rig.network.failAfterMs = 2500;
  rig.run();
  rig.expectFailure(StageId::Join, Detail::NotFound, Message::JoinFailed);
  EXPECT_EQ(rig.report().reason, 201);
}

TEST(ConnectionCheck, JoinAuthenticationFailed) {
  for (const int reason : {2, 15, 23, 202, 204}) {
    Rig rig;
    rig.network.joinAfterMs = -1;
    rig.network.reason = reason;
    rig.network.failAfterMs = 4000;
    rig.run();
    rig.expectFailure(StageId::Join, Detail::AuthFailed, Message::JoinFailed);
    EXPECT_EQ(rig.report().reason, reason);
  }
}

TEST(ConnectionCheck, JoinFailedForAnotherReason) {
  Rig rig;
  rig.network.joinAfterMs = -1;
  rig.network.reason = 205;
  rig.network.failAfterMs = 4000;
  rig.run();
  rig.expectFailure(StageId::Join, Detail::Failed, Message::JoinFailed);
  EXPECT_EQ(rig.report().reason, 205);
}

TEST(ConnectionCheck, JoinFailedWithoutAReason) {
  Rig rig;
  rig.network.joinAfterMs = -1;
  rig.network.failAfterMs = 4000;
  rig.run();
  rig.expectFailure(StageId::Join, Detail::Failed, Message::JoinFailed);
  EXPECT_EQ(rig.report().reason, 0);
}

TEST(ConnectionCheck, JoinTimesOutAtTwentySeconds) {
  Rig rig;
  rig.network.joinAfterMs = -1;
  rig.limits = {};
  rig.start();
  const uint32_t began = rig.time.ms;
  while (rig.check->running()) {
    rig.check->step();
    if (rig.check->running()) EXPECT_LT(rig.time.ms - began, 20000u);
    rig.time.ms += 1;
  }
  rig.expectFailure(StageId::Join, Detail::Timeout, Message::JoinFailed);
  EXPECT_EQ(rig.report().stage(StageId::Join).ms, 20000u);
}

TEST(ConnectionCheck, JoinTimeoutWithAReasonReportsTheReason) {
  Rig rig;
  rig.network.joinAfterMs = -1;
  rig.network.reason = 15;  // the driver keeps retrying a wrong passphrase
  rig.run();
  rig.expectFailure(StageId::Join, Detail::AuthFailed, Message::JoinFailed);
  EXPECT_EQ(rig.report().stage(StageId::Join).ms, 20000u);
}

TEST(ConnectionCheck, EapTlsJoinHasThirtySeconds) {
  Rig rig;
  rig.network.info.auth = "eap-tls";
  rig.network.joinAfterMs = 25000;
  rig.network.eapChecked = 0;
  rig.run();
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
  EXPECT_EQ(rig.report().eapTimeChecked, 0);

  Rig late;
  late.network.info.auth = "eap-tls";
  late.network.joinAfterMs = -1;
  late.run();
  late.expectFailure(StageId::Join, Detail::Timeout, Message::JoinFailed);
  EXPECT_EQ(late.report().stage(StageId::Join).ms, 30000u);
}

TEST(ConnectionCheck, AReasonFromARetriedAttemptIsForgottenOnceJoined) {
  Rig rig;
  rig.network.reason = 201;
  rig.network.joinAfterMs = 6000;
  rig.run();
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
  EXPECT_EQ(rig.report().reason, 0);
}

TEST(ConnectionCheck, JoinDetailMapping) {
  EXPECT_EQ(conncheck::joinDetail(201), Detail::NotFound);
  EXPECT_EQ(conncheck::joinDetail(212), Detail::NotFound);
  for (const int reason : {2, 15, 23, 202, 204}) EXPECT_EQ(conncheck::joinDetail(reason), Detail::AuthFailed);
  for (const int reason : {1, 3, 4, 8, 200, 203, 205, 210, 211})
    EXPECT_EQ(conncheck::joinDetail(reason), Detail::Failed);
}

// ---- network --------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, NoAddressWithinTenSeconds) {
  Rig rig;
  rig.network.addressAfterMs = -1;
  rig.run();
  rig.expectFailure(StageId::Network, Detail::Timeout, Message::NoAddress);
  EXPECT_EQ(rig.report().stage(StageId::Network).ms, 10000u);
}

TEST(ConnectionCheck, AnAddressWithoutGatewayOrDnsIsNoAddress) {
  Rig rig;
  rig.network.partialAddress = true;
  rig.run();
  rig.expectFailure(StageId::Network, Detail::NoAddress, Message::NoAddress);
}

TEST(ConnectionCheck, LinkLostBeforeAnAddress) {
  Rig rig;
  rig.network.addressAfterMs = -1;
  rig.network.linkDropAfterMs = 300;
  rig.run();
  rig.expectFailure(StageId::Network, Detail::NoAddress, Message::NoAddress);
  EXPECT_LT(rig.report().stage(StageId::Network).ms, 1000u);
}

// ---- dns ------------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, MailboxNameDoesNotResolve) {
  Rig rig;
  rig.http.resolveFails = true;
  rig.run();
  rig.expectFailure(StageId::Dns, Detail::Failed, Message::DnsFailed);
}

TEST(ConnectionCheck, NameLookupTimesOutAtEightSeconds) {
  Rig rig;
  rig.http.resolveAfterMs = -1;
  rig.run();
  rig.expectFailure(StageId::Dns, Detail::Timeout, Message::DnsFailed);
  EXPECT_EQ(rig.report().stage(StageId::Dns).ms, 8000u);
  EXPECT_EQ(rig.http.resolveAborts, 1);  // the lookup was abandoned, not left running
}

// ---- time -----------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, ClockFailsWithoutRetainedTime) {
  Rig rig;
  rig.time.syncFails = true;
  rig.run();
  rig.expectFailure(StageId::Time, Detail::Failed, Message::TimeFailed);
  EXPECT_EQ(rig.time.retainedCalls, 1);
}

TEST(ConnectionCheck, ClockTimesOutAtTwentySeconds) {
  Rig rig;
  rig.time.syncAfterMs = -1;
  rig.run();
  rig.expectFailure(StageId::Time, Detail::Timeout, Message::TimeFailed);
  EXPECT_EQ(rig.report().stage(StageId::Time).ms, 20000u);
}

TEST(ConnectionCheck, RetainedTimeStandsInWhenSntpFails) {
  Rig rig;
  rig.time.syncAfterMs = -1;
  rig.time.retainedTrusted = true;
  rig.run();
  rig.expectCleanedUp();
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
  EXPECT_EQ(rig.report().timeSource, clockprov::Source::Retained);
  EXPECT_EQ(rig.report().stage(StageId::Time).ms, 20000u);  // SNTP is preferred: it had its full time first
}

// ---- https ----------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, ConnectionToTheMailboxFails) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::Connect);
  rig.run();
  rig.expectFailure(StageId::Https, Detail::ConnectFailed, Message::MailboxUnreachable);
}

TEST(ConnectionCheck, CertificateFails) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::Certificate);
  rig.run();
  rig.expectFailure(StageId::Https, Detail::Certificate, Message::CertificateFailed);
}

TEST(ConnectionCheck, HandshakeFails) {
  for (const auto kind : {httpfail::Failure::Tls, httpfail::Failure::Preflight}) {
    Rig rig;
    rig.http.openResult = method::HttpOpen::Failed;
    rig.http.failure = failureOf(kind);
    rig.run();
    rig.expectFailure(StageId::Https, Detail::TlsFailed, Message::MailboxUnreachable);
  }
}

TEST(ConnectionCheck, HandshakeTimesOut) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::Timeout);
  rig.http.openTakesMs = 5000;
  rig.run();
  rig.expectFailure(StageId::Https, Detail::Timeout, Message::MailboxUnreachable);
}

TEST(ConnectionCheck, AConnectFailureThatOutlastsTheDeadlineIsATimeout) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::Connect);
  rig.http.openTakesMs = 15000;
  rig.run();
  rig.expectFailure(StageId::Https, Detail::Timeout, Message::MailboxUnreachable);
}

TEST(ConnectionCheck, HttpsGivesUpAtFifteenSecondsWhileTheHeapIsBusy) {
  Rig rig;
  rig.http.readyWaits = -1;
  rig.run();
  rig.expectFailure(StageId::Https, Detail::Timeout, Message::MailboxUnreachable);
  EXPECT_EQ(rig.report().stage(StageId::Https).ms, 15000u);
  EXPECT_EQ(rig.http.opens, 0);
}

TEST(ConnectionCheck, TooLittleMemoryForTls) {
  Rig rig;
  rig.http.noMemory = true;
  rig.run();
  rig.expectFailure(StageId::Https, Detail::TlsFailed, Message::MailboxUnreachable);
}

TEST(ConnectionCheck, RedirectIsReportedWithItsHostAndNotFollowed) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::Redirect, 302, "portal.example.edu");
  rig.run();
  rig.expectFailure(StageId::Https, Detail::Redirect, Message::MailboxUnreachable);
  EXPECT_STREQ(rig.report().locationHost, "portal.example.edu");
  EXPECT_EQ(rig.http.opens, 1);  // nothing was asked of the place it named
  const JsonDocument doc = parse(rig.json());
  EXPECT_STREQ(doc["stages"][4]["detail"], "redirect");
  EXPECT_STREQ(doc["stages"][4]["location_host"], "portal.example.edu");
}

TEST(ConnectionCheck, RedirectWithoutAHostPointsAtTheMailboxHost) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::Redirect, 301, "");
  rig.run();
  rig.expectFailure(StageId::Https, Detail::Redirect, Message::MailboxUnreachable);
  EXPECT_STREQ(rig.report().locationHost, HOST);
}

// ---- mailbox --------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, MailboxRejectsTheReader) {
  for (const int status : {401, 403}) {
    Rig rig;
    rig.http.openResult = method::HttpOpen::Unauthorized;
    rig.http.failure = failureOf(httpfail::Failure::Status, status);
    rig.run();
    rig.expectFailure(StageId::Mailbox, Detail::Unauthorized, Message::AccessRejected);
    EXPECT_EQ(rig.report().httpStatus, status);
    EXPECT_EQ(rig.report().stage(StageId::Mailbox).ms, 0u);
  }
}

TEST(ConnectionCheck, MailboxAnswersWithOtherStatuses) {
  struct Case {
    int status;
    method::HttpOpen open;
    Detail detail;
  };
  const Case cases[] = {{404, method::HttpOpen::Failed, Detail::NotFound},
                        {500, method::HttpOpen::Failed, Detail::ServerError},
                        {503, method::HttpOpen::Failed, Detail::ServerError},
                        {410, method::HttpOpen::Gone, Detail::UnexpectedResponse},
                        {418, method::HttpOpen::Failed, Detail::UnexpectedResponse},
                        {204, method::HttpOpen::Failed, Detail::UnexpectedResponse}};
  for (const Case& c : cases) {
    Rig rig;
    rig.http.openResult = c.open;
    rig.http.failure = failureOf(httpfail::Failure::Status, c.status);
    rig.run();
    rig.expectFailure(StageId::Mailbox, c.detail, Message::UnexpectedAnswer);
    EXPECT_EQ(rig.report().httpStatus, c.status);
    const JsonDocument doc = parse(rig.json());
    EXPECT_EQ(doc["stages"][5]["http_status"].as<int>(), c.status);
    EXPECT_STREQ(doc["stages"][5]["detail"], conncheck::detailName(c.detail));
  }
}

TEST(ConnectionCheck, OkWithoutAnAcceptableLengthIsUnexpected) {
  Rig rig;
  rig.http.openResult = method::HttpOpen::Failed;
  rig.http.failure = failureOf(httpfail::Failure::BadLength, 200);
  rig.run();
  rig.expectFailure(StageId::Mailbox, Detail::UnexpectedResponse, Message::UnexpectedAnswer);
  EXPECT_EQ(rig.report().httpStatus, 200);
}

TEST(ConnectionCheck, AReplyThatIsNotABookListIsUnexpected) {
  for (const std::string& body : {std::string("<html>Sign in to continue</html>"), std::string("{\"books\":7}"),
                                  std::string("{\"message_id\":null}")}) {
    Rig rig;
    rig.http.body = body;
    rig.run();
    rig.expectFailure(StageId::Mailbox, Detail::UnexpectedResponse, Message::UnexpectedAnswer);
  }
}

TEST(ConnectionCheck, AnEmptyListIsAValidList) {
  Rig rig;
  rig.http.body = "{\"books\":[]}";
  rig.run();
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
}

TEST(ConnectionCheck, ATruncatedOrBrokenReplyIsUnexpected) {
  Rig cut;
  cut.http.length = static_cast<int64_t>(cut.http.body.size()) + 50;  // announced more than came
  cut.run();
  cut.expectFailure(StageId::Mailbox, Detail::UnexpectedResponse, Message::UnexpectedAnswer);

  Rig broken;
  broken.http.failMidway = true;
  broken.run();
  broken.expectFailure(StageId::Mailbox, Detail::UnexpectedResponse, Message::UnexpectedAnswer);

  Rig empty;
  empty.http.length = 0;
  empty.run();
  empty.expectFailure(StageId::Mailbox, Detail::UnexpectedResponse, Message::UnexpectedAnswer);
}

TEST(ConnectionCheck, AStalledReplyTimesOutAtTenSeconds) {
  Rig rig;
  rig.http.stallAt = 100;
  rig.run();
  rig.expectFailure(StageId::Mailbox, Detail::Timeout, Message::UnexpectedAnswer);
  EXPECT_EQ(rig.report().stage(StageId::Mailbox).ms, 10000u);
}

// ---- deadlines, exactly ---------------------------------------------------------------------------------------------

TEST(ConnectionCheck, EveryDeadlineIsExact) {
  struct Case {
    StageId stage;
    uint32_t limit;
    void (*arrange)(Rig&);
  };
  const Case cases[] = {
      {StageId::Join, 20000, [](Rig& r) { r.network.joinAfterMs = -1; }},
      {StageId::Network, 10000, [](Rig& r) { r.network.addressAfterMs = -1; }},
      {StageId::Dns, 8000, [](Rig& r) { r.http.resolveAfterMs = -1; }},
      {StageId::Time, 20000, [](Rig& r) { r.time.syncAfterMs = -1; }},
      {StageId::Https, 15000, [](Rig& r) { r.http.readyWaits = -1; }},
      {StageId::Mailbox, 10000, [](Rig& r) { r.http.stallAt = 10; }},
  };
  for (const Case& c : cases) {
    Rig rig;
    c.arrange(rig);
    rig.runUntil(c.stage);
    // Still running 1 ms before the limit, failed at it.
    const uint32_t started = rig.enteredAt;
    rig.time.ms = started + c.limit - 1;
    rig.check->step();
    EXPECT_TRUE(rig.check->running()) << conncheck::stageName(c.stage);
    rig.time.ms = started + c.limit;
    rig.check->step();
    EXPECT_FALSE(rig.check->running()) << conncheck::stageName(c.stage);
    EXPECT_EQ(rig.status(c.stage), Status::Failed) << conncheck::stageName(c.stage);
    EXPECT_EQ(rig.detail(c.stage), Detail::Timeout) << conncheck::stageName(c.stage);
    EXPECT_EQ(rig.report().stage(c.stage).ms, c.limit) << conncheck::stageName(c.stage);
    rig.expectCleanedUp();
  }
}

// ---- cancel ---------------------------------------------------------------------------------------------------------

TEST(ConnectionCheck, CancelInEveryStage) {
  const StageId stages[] = {StageId::Join, StageId::Network, StageId::Dns,
                            StageId::Time, StageId::Https,   StageId::Mailbox};
  for (const StageId stage : stages) {
    Rig rig;
    // Hold every stage open so the check waits in the one under test.
    rig.network.joinAfterMs = stage == StageId::Join ? -1 : 3000;
    rig.network.addressAfterMs = stage == StageId::Network ? -1 : 800;
    rig.http.resolveAfterMs = stage == StageId::Dns ? -1 : 150;
    rig.time.syncAfterMs = stage == StageId::Time ? -1 : 1200;
    rig.http.readyWaits = stage == StageId::Https ? -1 : 0;
    rig.http.stallAt = stage == StageId::Mailbox ? 10 : SIZE_MAX;
    rig.runUntil(stage);
    rig.check->step();
    rig.time.ms += 250;
    rig.check->cancel();  // Back: takes effect at once, within the pass that saw it
    rig.expectCleanedUp();
    EXPECT_EQ(rig.report().outcome, Outcome::Canceled) << conncheck::stageName(stage);
    EXPECT_EQ(rig.report().message, Message::Canceled);
    rig.expectEarlierOk(stage);
    EXPECT_EQ(rig.status(stage), Status::Canceled) << conncheck::stageName(stage);
    EXPECT_GE(rig.report().stage(stage).ms, 250u);
    rig.expectLaterNotRun(stage);
    EXPECT_EQ(rig.log.lines.back().rfind(std::string(conncheck::stageName(stage)) + " canceled", 0), 0u);
  }
}

TEST(ConnectionCheck, CancelAfterTheEndChangesNothing) {
  Rig rig;
  rig.run();
  const uint32_t revision = rig.check->revision();
  rig.check->cancel();
  EXPECT_EQ(rig.check->revision(), revision);
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
}

TEST(ConnectionCheck, DestroyingARunningCheckCleansUp) {
  Rig rig;
  rig.network.addressAfterMs = -1;
  rig.runUntil(StageId::Network);
  rig.check.reset();
  EXPECT_FALSE(rig.network.radioOn);
  EXPECT_FALSE(rig.time.guardHeld);
  EXPECT_FALSE(rig.http.connectionOpen);
  EXPECT_FALSE(rig.http.lookupPending);
}

TEST(ConnectionCheck, ARunCanBeRepeated) {
  Rig rig;
  rig.http.resolveFails = true;
  rig.run();
  EXPECT_EQ(rig.report().outcome, Outcome::Failed);
  rig.http.resolveFails = false;
  rig.check->start();
  while (rig.check->running()) {
    rig.check->step();
    rig.time.ms += STEP_MS;
  }
  rig.expectCleanedUp();
  EXPECT_EQ(rig.report().outcome, Outcome::Ok);
  EXPECT_EQ(rig.report().stage(StageId::Dns).detail, Detail::None);
}

// ---- the result file ------------------------------------------------------------------------------------------------

TEST(ConnectionCheckResult, ShapeOfASuccessfulCheck) {
  Rig rig;
  rig.run();
  const std::string text = rig.json();
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(text.back(), '\n');
  const JsonDocument doc = parse(text);
  EXPECT_EQ(doc["schema"].as<int>(), 1);
  EXPECT_STREQ(doc["kind"], "plaque-connection-check");
  EXPECT_EQ(doc["finished_at"].as<long long>(), 1792000123LL);
  EXPECT_STREQ(doc["clock"], "sntp");
  EXPECT_EQ(doc["boot_ms"].as<int>(), 48211);
  EXPECT_STREQ(doc["reader_uuid"], "0b6c9f9e-3c1a-4c7e-9f00-1d2e3f4a5b6c");
  EXPECT_STREQ(doc["model"], "x4c");
  EXPECT_STREQ(doc["firmware"], "1.6.5-x4c+2a6befc");
  EXPECT_STREQ(doc["station_mac"], "AA:BB:CC:DD:EE:FF");
  EXPECT_STREQ(doc["network"]["source"], "saved");
  EXPECT_STREQ(doc["network"]["name"], "Lab 100");
  EXPECT_STREQ(doc["network"]["ssid"], "Lab 100");
  EXPECT_STREQ(doc["network"]["auth"], "psk");
  EXPECT_STREQ(doc["mailbox_host"], HOST);
  EXPECT_STREQ(doc["result"], "ok");
  EXPECT_STREQ(doc["message"], "Connected to the mailbox.");
  const JsonArrayConst stages = doc["stages"];
  ASSERT_EQ(stages.size(), 6u);
  const char* ids[] = {"join", "network", "dns", "time", "https", "mailbox"};
  for (size_t i = 0; i < 6; ++i) {
    EXPECT_STREQ(stages[i]["id"], ids[i]);
    EXPECT_STREQ(stages[i]["status"], "ok");
    EXPECT_TRUE(stages[i]["ms"].is<int>());
    EXPECT_FALSE(stages[i]["detail"].is<const char*>());
  }
  EXPECT_EQ(stages[0]["rssi"].as<int>(), -61);
  EXPECT_EQ(stages[0]["channel"].as<int>(), 6);
  EXPECT_TRUE(stages[0]["eap_time_checked"].isNull());
  EXPECT_NE(text.find("\"eap_time_checked\":null"), std::string::npos);  // present, as null
  EXPECT_STREQ(stages[1]["ip"], "10.1.2.3");
  EXPECT_STREQ(stages[1]["gateway"], "10.1.0.1");
  EXPECT_EQ(stages[1]["dns"].size(), 2u);
  EXPECT_STREQ(stages[1]["dns"][0], "10.0.0.53");
  EXPECT_EQ(stages[5]["http_status"].as<int>(), 200);
}

TEST(ConnectionCheckResult, ShapeOfTheContractExample) {
  Rig rig;
  rig.http.resolveAfterMs = -1;
  rig.run();
  const JsonDocument doc = parse(rig.json());
  EXPECT_STREQ(doc["result"], "failed");
  EXPECT_STREQ(doc["message"], "Wi-Fi joined. Couldn't resolve the mailbox.");
  const JsonArrayConst stages = doc["stages"];
  EXPECT_STREQ(stages[2]["status"], "failed");
  EXPECT_EQ(stages[2]["ms"].as<int>(), 8000);
  EXPECT_STREQ(stages[2]["detail"], "timeout");
  for (size_t i = 3; i < 6; ++i) {
    EXPECT_STREQ(stages[i]["status"], "not_run");
    EXPECT_EQ(stages[i].as<JsonObjectConst>().size(), 2u);  // just id and status
  }
}

TEST(ConnectionCheckResult, FinishedAtIsNullWithoutTrustedTime) {
  Rig rig;
  rig.time.syncFails = true;
  rig.run();
  const std::string text = rig.json(0);
  const JsonDocument doc = parse(text);
  EXPECT_TRUE(doc["finished_at"].isNull());
  EXPECT_NE(text.find("\"finished_at\":null"), std::string::npos);  // present, as null
  EXPECT_STREQ(doc["clock"], "none");
  EXPECT_STREQ(doc["stages"][3]["detail"], "failed");
}

TEST(ConnectionCheckResult, NeverCarriesTokensPasswordsOrBookTitles) {
  Rig rig;
  rig.run();
  const std::string ok = rig.json();
  EXPECT_EQ(ok.find(TOKEN), std::string::npos);
  EXPECT_EQ(ok.find(PSK), std::string::npos);
  EXPECT_EQ(ok.find(SECRET_TITLE), std::string::npos);
  EXPECT_EQ(ok.find("Bearer"), std::string::npos);
  EXPECT_EQ(ok.find("book"), std::string::npos);  // not even the list's keys

  Rig rejected;
  rejected.http.openResult = method::HttpOpen::Unauthorized;
  rejected.http.failure = failureOf(httpfail::Failure::Status, 401);
  rejected.run();
  const std::string refused = rejected.json();
  EXPECT_EQ(refused.find(TOKEN), std::string::npos);
  EXPECT_EQ(refused.find(PSK), std::string::npos);
}

TEST(ConnectionCheckResult, NotSetUpHasNoMailboxHostAndNoStageResults) {
  Rig rig;
  rig.config.configured = false;
  rig.run();
  const JsonDocument doc = parse(rig.json());
  EXPECT_STREQ(doc["result"], "not_set_up");
  EXPECT_TRUE(doc["mailbox_host"].isNull());
  for (size_t i = 0; i < 6; ++i) EXPECT_STREQ(doc["stages"][i]["status"], "not_run");
}

TEST(ConnectionCheckResult, NetworkIsOmittedWhenNothingIsConfigured) {
  Rig rig;
  rig.network.info = conncheck::NetworkInfo{};
  rig.run();
  const JsonDocument doc = parse(rig.json());
  EXPECT_FALSE(doc["network"].is<JsonObjectConst>());
  EXPECT_EQ(rig.json().find("\"network\":{"), std::string::npos);
  EXPECT_STREQ(doc["stages"][0]["detail"], "nothing_configured");
}

TEST(ConnectionCheckResult, JoinFailureCarriesTheDriverReason) {
  Rig rig;
  rig.network.joinAfterMs = -1;
  rig.network.reason = 202;
  rig.network.failAfterMs = 1000;
  rig.run();
  const JsonDocument doc = parse(rig.json());
  EXPECT_STREQ(doc["stages"][0]["detail"], "auth_failed");
  EXPECT_EQ(doc["stages"][0]["reason"].as<int>(), 202);
  EXPECT_FALSE(doc["stages"][0]["rssi"].is<int>());
}

TEST(ConnectionCheckResult, CanceledCheck) {
  Rig rig;
  rig.time.syncAfterMs = -1;
  rig.runUntil(StageId::Time);
  rig.check->cancel();
  const JsonDocument doc = parse(rig.json());
  EXPECT_STREQ(doc["result"], "canceled");
  EXPECT_STREQ(doc["message"], "Check canceled.");
  EXPECT_STREQ(doc["stages"][3]["status"], "canceled");
  EXPECT_TRUE(doc["stages"][3]["ms"].is<int>());
  EXPECT_STREQ(doc["stages"][4]["status"], "not_run");
}

TEST(ConnectionCheckResult, EapTimeCheckedIsWrittenAsABoolean) {
  Rig rig;
  rig.network.info.auth = "eap-tls";
  rig.network.eapChecked = 1;
  rig.run();
  const JsonDocument doc = parse(rig.json());
  EXPECT_TRUE(doc["stages"][0]["eap_time_checked"].as<bool>());
  EXPECT_TRUE(doc["stages"][0]["eap_time_checked"].is<bool>());
}

TEST(ConnectionCheckResult, NamesAreEscapedAndKeptValidUtf8) {
  Rig rig;
  rig.network.info.name = "Lab \"100\" \\ caf\xC3\xA9";
  rig.network.info.ssid = std::string("bad\xFF\xC3", 5) + "\n";
  rig.run();
  const std::string text = rig.json();
  const JsonDocument doc = parse(text);
  EXPECT_STREQ(doc["network"]["name"], "Lab \"100\" \\ caf\xC3\xA9");
  EXPECT_STREQ(doc["network"]["ssid"], "bad\xEF\xBF\xBD\xEF\xBF\xBD\n");
}

TEST(ConnectionCheckResult, MissingIdentityIsNull) {
  Rig rig;
  rig.run();
  conncheck::ResultContext context;
  const JsonDocument doc = parse(conncheck::resultJson(context, rig.report()));
  EXPECT_TRUE(doc["reader_uuid"].isNull());
  EXPECT_TRUE(doc["station_mac"].isNull());
  EXPECT_TRUE(doc["finished_at"].isNull());
}

TEST(ConnectionCheckResult, MacText) {
  const uint8_t mac[6] = {0xaa, 0xbb, 0x0c, 0x0d, 0xee, 0xf0};
  EXPECT_EQ(conncheck::macText(mac), "AA:BB:0C:0D:EE:F0");
}

TEST(ConnectionCheckResult, Names) {
  EXPECT_STREQ(conncheck::statusName(Status::Pending), "not_run");
  EXPECT_STREQ(conncheck::statusName(Status::Running), "not_run");
  EXPECT_STREQ(conncheck::outcomeName(Outcome::NotSetUp), "not_set_up");
  EXPECT_STREQ(conncheck::detailName(Detail::ConnectFailed), "connect_failed");
  EXPECT_STREQ(conncheck::detailName(Detail::TlsFailed), "tls_failed");
  EXPECT_STREQ(conncheck::detailName(Detail::UnexpectedResponse), "unexpected_response");
  EXPECT_STREQ(conncheck::messageText(Message::CertificateFailed), "Couldn't verify the server certificate.");
  EXPECT_STREQ(conncheck::messageText(Message::AccessRejected), "Mailbox access was rejected.");
}
