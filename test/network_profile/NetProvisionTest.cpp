#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "NetCodec.h"
#include "NetProvisionProtocol.h"
#include "NetworkProfile.h"
#include "TestBundles.h"

using netprofile::Error;
using netprov::Session;

namespace {

constexpr char P[] = "PLAQUE-NET/1 ";

class FakeHost final : public netprov::Host {
 public:
  std::vector<std::string> sent;
  std::string info = "{\"reader_uuid\": \"x\"}";
  std::string profile = "{\"schema\": 1, \"installed\": false}";
  bool hasResult = false;
  std::string result;
  Error arrival = Error::None;
  std::string received;
  int arrivals = 0;
  bool removeAsks = true;
  int removeCalls = 0;
  std::vector<Ended> ended;  // every question dropped without an answer

  void questionEnded(const Ended why) override { ended.push_back(why); }
  void sendLine(std::string_view line) override { sent.emplace_back(line); }
  bool readerInfo(std::string& out) override {
    out = info;
    return true;
  }
  bool profileSummary(std::string& out) override {
    out = profile;
    return true;
  }
  bool checkResult(std::string& out) override {
    out = result;
    return hasResult;
  }
  Error bundleArrived(const uint8_t* data, size_t size) override {
    ++arrivals;
    received.assign(reinterpret_cast<const char*>(data), size);
    return arrival;
  }
  bool removeArrived() override {
    ++removeCalls;
    return removeAsks;
  }
  std::string last() const { return sent.empty() ? std::string() : sent.back(); }
};

struct Rig {
  FakeHost host;
  std::vector<uint8_t> buffer = std::vector<uint8_t>(netprofile::MAX_BUNDLE_BYTES);
  Session session{host, buffer.data(), buffer.size()};
  uint32_t now = 1000;

  void line(const std::string& text) {
    const std::string full = text + "\n";
    session.feed(reinterpret_cast<const uint8_t*>(full.data()), full.size(), now);
  }
  void command(const std::string& text) { line(std::string(P) + text); }
  // PUT, DATA in lines of at most 256 characters, END.
  void send(const std::string& payload) {
    uint8_t sha[32];
    netcodec::sha256(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), sha);
    char hex[65];
    netcodec::toHex(sha, 32, hex);
    command("PUT " + std::to_string(payload.size()) + " " + hex);
    const std::string b64 = netcodec::base64Encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    for (size_t i = 0; i < b64.size(); i += 256) command("DATA " + b64.substr(i, 256));
    command("END");
  }
};

std::string reply(const std::string& text) { return std::string(P) + text; }

}  // namespace

TEST(NetProvision, NoiseLinesAreIgnored) {
  Rig rig;
  rig.line("I (123) METHOD: phase join: start");
  rig.line("");
  rig.line("PLAQUE-NET/2 HELLO");  // another version is somebody else's protocol
  rig.line("plaque-net/1 hello");
  EXPECT_TRUE(rig.host.sent.empty());
}

TEST(NetProvision, HelloAnswersReadyWithTheLimit) {
  Rig rig;
  rig.command("HELLO");
  EXPECT_EQ(reply("READY 16384"), rig.host.last());
}

TEST(NetProvision, InfoProfileAndResultCarryTheReadersFilesInBase64) {
  Rig rig;
  rig.command("INFO");
  const std::string expected =
      netcodec::base64Encode(reinterpret_cast<const uint8_t*>(rig.host.info.data()), rig.host.info.size());
  EXPECT_EQ(reply("INFO " + expected), rig.host.last());
  rig.command("PROFILE");
  EXPECT_EQ(0u, rig.host.last().find(reply("PROFILE ")));
  rig.command("RESULT");
  EXPECT_EQ(reply("RESULT none"), rig.host.last());
  rig.host.hasResult = true;
  rig.host.result = "{\"schema\": 1}";
  rig.command("RESULT");
  EXPECT_EQ(0u, rig.host.last().find(reply("RESULT ")));
  EXPECT_NE(reply("RESULT none"), rig.host.last());
}

TEST(NetProvision, ABundleArrivesAsksAndInstalls) {
  Rig rig;
  const std::string bundle = testbundle::pskBundle().text();
  rig.send(bundle);
  EXPECT_EQ(1, rig.host.arrivals);
  EXPECT_EQ(bundle, rig.host.received);
  EXPECT_EQ(reply("CONFIRM"), rig.host.last());
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  rig.session.installDecided(Error::None);
  EXPECT_EQ(reply("OK installed"), rig.host.last());
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, ThePersonMayDecline) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  rig.session.installDecided(Error::Declined);
  EXPECT_EQ(reply("ERR declined"), rig.host.last());
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, AValidationErrorIsAnsweredWithItsCode) {
  Rig rig;
  rig.host.arrival = Error::KeyMismatch;
  rig.send(testbundle::openBundle().text());
  EXPECT_EQ(reply("ERR key_mismatch"), rig.host.last());
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, AWrongChecksumIsRefusedAndNothingArrives) {
  Rig rig;
  const std::string bundle = testbundle::openBundle().text();
  const std::string wrong(64, '0');
  rig.command("PUT " + std::to_string(bundle.size()) + " " + wrong);
  const std::string b64 = netcodec::base64Encode(reinterpret_cast<const uint8_t*>(bundle.data()), bundle.size());
  for (size_t i = 0; i < b64.size(); i += 256) rig.command("DATA " + b64.substr(i, 256));
  rig.command("END");
  EXPECT_EQ(reply("ERR checksum"), rig.host.last());
  EXPECT_EQ(0, rig.host.arrivals);
}

TEST(NetProvision, ALengthThatDoesNotMatchIsAChecksumError) {
  Rig rig;
  const std::string bundle = testbundle::openBundle().text();
  uint8_t sha[32];
  netcodec::sha256(reinterpret_cast<const uint8_t*>(bundle.data()), bundle.size(), sha);
  char hex[65];
  netcodec::toHex(sha, 32, hex);
  rig.command("PUT " + std::to_string(bundle.size() + 5) + " " + hex);
  const std::string b64 = netcodec::base64Encode(reinterpret_cast<const uint8_t*>(bundle.data()), bundle.size());
  for (size_t i = 0; i < b64.size(); i += 256) rig.command("DATA " + b64.substr(i, 256));
  rig.command("END");
  EXPECT_EQ(reply("ERR checksum"), rig.host.last());
  EXPECT_EQ(0, rig.host.arrivals);
}

TEST(NetProvision, ATransferOverTheLimitIsRefusedUpFront) {
  Rig rig;
  rig.command("PUT " + std::to_string(netprofile::MAX_BUNDLE_BYTES + 1) + " " + std::string(64, 'a'));
  EXPECT_EQ(reply("ERR too_large"), rig.host.last());
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, BadBase64IsAnErrorNotACrash) {
  Rig rig;
  rig.command("PUT 10 " + std::string(64, 'a'));
  rig.command("DATA ***not-base64***");
  EXPECT_EQ(0u, rig.host.last().find(reply("ERR ")));
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  rig.command("END");
  EXPECT_EQ(0, rig.host.arrivals);
}

TEST(NetProvision, ADataLineLongerThanTheLimitIsRefused) {
  Rig rig;
  rig.command("PUT 400 " + std::string(64, 'a'));
  rig.command("DATA " + std::string(300, 'A'));
  EXPECT_EQ(0u, rig.host.last().find(reply("ERR ")));
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, ASilentTransferIsDroppedAndTheNextLineAnsweredBusy) {
  Rig rig;
  rig.command("PUT 100 " + std::string(64, 'a'));
  EXPECT_EQ(Session::State::Receiving, rig.session.state());
  rig.now += netprov::STALL_MS - 1;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::Receiving, rig.session.state());
  rig.now += 2;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  rig.command("HELLO");
  EXPECT_EQ(reply("ERR busy"), rig.host.last());
  rig.command("HELLO");
  EXPECT_EQ(reply("READY 16384"), rig.host.last());
}

TEST(NetProvision, WhileThePersonDecidesEveryLineIsBusy) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  rig.command("HELLO");
  EXPECT_EQ(reply("ERR busy"), rig.host.last());
  rig.command("INFO");
  EXPECT_EQ(reply("ERR busy"), rig.host.last());
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  rig.session.installDecided(Error::None);
  EXPECT_EQ(reply("OK installed"), rig.host.last());
}

// ---- a question nobody answers does not wait for ever
// -------------------------------------------------------------------

TEST(NetProvision, AnInstallQuestionNobodyAnswersIsDeclinedAfterTwoMinutes) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  ASSERT_EQ(Session::State::AwaitingInstall, rig.session.state());
  const uint32_t asked = rig.now;
  // Until then it is genuinely up: every line is busy, and the question stands.
  rig.now = asked + netprov::QUESTION_TIMEOUT_MS - 1;
  rig.session.tick(rig.now);
  rig.command("HELLO");
  EXPECT_EQ(reply("ERR busy"), rig.host.last());
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  EXPECT_TRUE(rig.host.ended.empty());
  // Then it is Back.
  rig.now = asked + netprov::QUESTION_TIMEOUT_MS;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  EXPECT_EQ(reply("ERR declined"), rig.host.last());
  EXPECT_EQ((std::vector<netprov::Host::Ended>{netprov::Host::Ended::TimedOut}), rig.host.ended);
  // The reader answers again, and a late answer from the screen changes nothing.
  rig.command("HELLO");
  EXPECT_EQ(reply("READY 16384"), rig.host.last());
  const size_t sent = rig.host.sent.size();
  rig.session.installDecided(Error::None);
  EXPECT_EQ(sent, rig.host.sent.size());
  EXPECT_EQ(1u, rig.host.ended.size());
}

TEST(NetProvision, ARemoveQuestionNobodyAnswersIsDeclinedToo) {
  Rig rig;
  rig.now += 50000;  // not the clock's start: the question's own time is what counts
  rig.command("REMOVE");
  ASSERT_EQ(Session::State::AwaitingRemove, rig.session.state());
  rig.now += netprov::QUESTION_TIMEOUT_MS - 1;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::AwaitingRemove, rig.session.state());
  rig.now += 1;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  EXPECT_EQ(reply("ERR declined"), rig.host.last());
  EXPECT_EQ((std::vector<netprov::Host::Ended>{netprov::Host::Ended::TimedOut}), rig.host.ended);
}

TEST(NetProvision, WorkingTheQuestionStartsTheTimeAgain) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  rig.now += netprov::QUESTION_TIMEOUT_MS - 1000;
  rig.session.questionTouched(rig.now);  // the person moved the highlight
  rig.now += netprov::QUESTION_TIMEOUT_MS - 1;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  rig.now += 1;
  rig.session.tick(rig.now);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  // Touching a question that is not up does nothing.
  rig.session.questionTouched(rig.now);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, AQuestionWhoseLinkIsGoneIsDroppedWithoutAReply) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  const size_t sent = rig.host.sent.size();
  rig.session.tick(rig.now, false);  // the cable comes out
  rig.session.tick(rig.now + netprov::LINK_GRACE_MS - 1, false);
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  rig.session.tick(rig.now + netprov::LINK_GRACE_MS, false);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  EXPECT_EQ(sent, rig.host.sent.size());  // nobody to hear it
  EXPECT_EQ((std::vector<netprov::Host::Ended>{netprov::Host::Ended::LinkLost}), rig.host.ended);
}

TEST(NetProvision, AFlappingLinkDoesNotDropAQuestion) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  uint32_t at = rig.now;
  for (int i = 0; i < 10; ++i) {
    rig.session.tick(at, false);
    at += netprov::LINK_GRACE_MS - 500;
    rig.session.tick(at, false);
    rig.session.tick(at, true);  // back before the grace is out
    at += 100;
  }
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  EXPECT_TRUE(rig.host.ended.empty());
}

TEST(NetProvision, ARemoveQuestionWhoseLinkIsGoneIsDroppedToo) {
  Rig rig;
  rig.command("REMOVE");
  rig.session.tick(rig.now, false);
  rig.session.tick(rig.now + netprov::LINK_GRACE_MS, false);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  EXPECT_EQ((std::vector<netprov::Host::Ended>{netprov::Host::Ended::LinkLost}), rig.host.ended);
}

TEST(NetProvision, ATransferWhoseLinkIsGoneIsDroppedAndItsBytesWiped) {
  Rig rig;
  rig.command("PUT 100 " + std::string(64, 'a'));
  rig.command("DATA QUJDREVGRw==");
  ASSERT_EQ(Session::State::Receiving, rig.session.state());
  rig.session.tick(rig.now, false);
  rig.session.tick(rig.now + netprov::LINK_GRACE_MS, false);
  EXPECT_EQ(Session::State::Idle, rig.session.state());
  EXPECT_TRUE(rig.host.ended.empty());  // no question was up
  for (const uint8_t byte : rig.buffer) ASSERT_EQ(0, byte);
}

TEST(NetProvision, ALinkThatIsUpNeverDropsAnythingByItself) {
  Rig rig;
  rig.send(testbundle::openBundle().text());
  rig.now += netprov::QUESTION_TIMEOUT_MS - 1;
  rig.session.tick(rig.now, true);
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
  EXPECT_TRUE(rig.host.ended.empty());
}

TEST(NetProvision, RemoveAsksThenAnswers) {
  Rig rig;
  rig.command("REMOVE");
  EXPECT_EQ(Session::State::AwaitingRemove, rig.session.state());
  rig.session.removeDecided(Error::None);
  EXPECT_EQ(reply("OK removed"), rig.host.last());
  rig.command("REMOVE");
  rig.session.removeDecided(Error::Declined);
  EXPECT_EQ(reply("ERR declined"), rig.host.last());
}

TEST(NetProvision, RemoveWithNothingInstalledIsAnsweredAtOnce) {
  Rig rig;
  rig.host.removeAsks = false;
  rig.command("REMOVE");
  EXPECT_EQ(reply("OK removed"), rig.host.last());
  EXPECT_EQ(Session::State::Idle, rig.session.state());
}

TEST(NetProvision, ClosingWhileAskedAnswersDeclinedAndWipesTheBuffer) {
  Rig rig;
  rig.send(testbundle::pskBundle().text());
  rig.session.close();
  EXPECT_EQ(reply("ERR declined"), rig.host.last());
  for (const uint8_t byte : rig.buffer) ASSERT_EQ(0, byte);
}

TEST(NetProvision, TheBufferIsWipedAfterEveryTransfer) {
  Rig rig;
  rig.send(testbundle::pskBundle().text());
  rig.session.installDecided(Error::None);
  for (const uint8_t byte : rig.buffer) ASSERT_EQ(0, byte);
}

TEST(NetProvision, BytesMayArriveInAnyPieces) {
  Rig whole;
  whole.send(testbundle::pskBundle().text());
  Rig bytes;
  uint8_t sha[32];
  const std::string payload = testbundle::pskBundle().text();
  netcodec::sha256(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), sha);
  char hex[65];
  netcodec::toHex(sha, 32, hex);
  std::string stream = std::string(P) + "PUT " + std::to_string(payload.size()) + " " + hex + "\r\n";
  const std::string b64 = netcodec::base64Encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  for (size_t i = 0; i < b64.size(); i += 100) stream += std::string(P) + "DATA " + b64.substr(i, 100) + "\r\n";
  stream += std::string(P) + "END\r\n";
  for (const char c : stream) bytes.session.feed(reinterpret_cast<const uint8_t*>(&c), 1, bytes.now);
  EXPECT_EQ(whole.host.received, bytes.host.received);
  EXPECT_EQ(reply("CONFIRM"), bytes.host.last());
}

TEST(NetProvision, NothingReceivedIsEverEchoed) {
  Rig rig;
  rig.send(testbundle::pskBundle().text());
  rig.session.installDecided(Error::None);
  rig.host.arrival = Error::PskInvalid;
  rig.send(testbundle::pskBundle().text());
  rig.command("HELLO");
  for (const std::string& line : rig.host.sent) {
    EXPECT_EQ(std::string::npos, line.find(testbundle::PSK)) << line;
    EXPECT_EQ(0u, line.find(P)) << line;
    EXPECT_LE(line.size(), 400u);
  }
}

TEST(NetProvision, SerialInputIsClaimedOnlyWhileTheScreenIsOpen) {
  EXPECT_FALSE(netprov::serialInputClaimed());
  netprov::claimSerialInput(true);
  EXPECT_TRUE(netprov::serialInputClaimed());
  netprov::claimSerialInput(false);
  EXPECT_FALSE(netprov::serialInputClaimed());
}

// ---- the USB receive queue is sized for a whole transfer
// ------------------------------------------------------------------

namespace {
// What a sender puts on the port for a payload: PUT, a DATA line per 256 base64 characters, END, each line ended with
// `ending`, with no pacing between them.
size_t wireBytes(const std::string& payload, const std::string& ending) {
  uint8_t sha[32];
  netcodec::sha256(reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), sha);
  char hex[65];
  netcodec::toHex(sha, 32, hex);
  size_t total = (std::string(P) + "PUT " + std::to_string(payload.size()) + " " + hex + ending).size();
  const std::string b64 = netcodec::base64Encode(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
  for (size_t i = 0; i < b64.size(); i += netprov::MAX_DATA_CHARS)
    total += (std::string(P) + "DATA " + b64.substr(i, netprov::MAX_DATA_CHARS) + ending).size();
  total += (std::string(P) + "END" + ending).size();
  return total;
}
}  // namespace

TEST(NetProvision, TheReceiveQueueHoldsTheLargestLegalTransferWhole) {
  // The interrupt of the Arduino core's USB Serial/JTAG driver throws away what its queue cannot hold and never makes
  // the host wait, so the queue has to be as big as the biggest transfer (a stalled loop task cannot lose a byte).
  const std::string biggest(netprofile::MAX_BUNDLE_BYTES, 'x');
  EXPECT_LE(wireBytes(biggest, "\n"), netprov::MAX_TRANSFER_WIRE_BYTES);
  EXPECT_LE(wireBytes(biggest, "\r\n"), netprov::MAX_TRANSFER_WIRE_BYTES);  // a sender that writes CRLF
  EXPECT_LT(wireBytes(biggest, "\r\n"), netprov::RX_QUEUE_BYTES);
  EXPECT_GE(netprov::RX_QUEUE_BYTES, netprov::MAX_TRANSFER_WIRE_BYTES);
  // Not wastefully big: internal RAM, held while the screen is open.
  EXPECT_LT(netprov::RX_QUEUE_BYTES, netprov::MAX_TRANSFER_WIRE_BYTES + 1024);
  EXPECT_EQ(21848u, netprov::MAX_BASE64_CHARS);
  EXPECT_EQ(86u, netprov::MAX_DATA_LINES);
  // And the whole of it is accepted by a session as one unpaced burst.
  Rig rig;
  const std::string bundle = testbundle::pskBundle().text();
  rig.send(bundle);
  EXPECT_EQ(Session::State::AwaitingInstall, rig.session.state());
}
