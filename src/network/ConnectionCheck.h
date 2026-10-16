#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "ClockProvenance.h"
#include "HttpFailure.h"
#include "ambient/MethodPorts.h"

// Settings › System › Check connection (SPEC-UNIVERSITY-WIFI section 8): six stages, each with a deadline, each
// reported as ok, failed, canceled or not run.
//
//   join     20 s (30 s for EAP-TLS)   associated and authenticated to the configured network
//   network  10 s                      an IPv4 address, a gateway and a DNS server
//   dns       8 s                      the mailbox host resolves
//   time     20 s                      trusted time: an SNTP answer, or the retained clock (ClockProvenance.h)
//   https    15 s                      TLS to the mailbox host with a verified chain, name and dates
//   mailbox  10 s                      GET /api/device/<id>/books answers 200 with a list the parser accepts
//
// A step machine in the style of method::Check: the screen calls step() once per main-loop pass and reads input
// between steps. No step waits, except the request open (connect, TLS handshake, headers), each part bounded by the
// 5 s socket timeout. Redirects are never followed; a 3xx is the https stage's "redirect", with the Location host.
// The https stage is the request's open (connect, handshake, headers); a local-development http origin has no TLS, so
// there it is the plain connection.
//
// Every way out goes through finish(): the name lookup is abandoned, the connection closed (its power-save guard
// with it), the clock sync abandoned (its guard too), the join abandoned and the radio switched off, and the buffers
// freed, before finish() returns. cancel() is that path for "the user left".
//
// Pure logic over the ports below, covered by test/connection_check with a fake clock and fake network.
namespace conncheck {

enum class StageId : uint8_t { Join, Network, Dns, Time, Https, Mailbox };
constexpr size_t STAGE_COUNT = 6;
const char* stageName(StageId stage);  // "join", "network", "dns", "time", "https", "mailbox"

// Pending and Running exist only while the check runs; a stage the check never reached ends NotRun.
enum class Status : uint8_t { Pending, Running, Ok, Failed, Canceled, NotRun };
const char* statusName(Status status);  // "ok", "failed", "canceled", "not_run"

enum class Detail : uint8_t {
  None,
  NothingConfigured,   // join
  NotFound,            // join: the reader did not see the network; mailbox: 404
  AuthFailed,          // join
  Timeout,             // any stage
  Failed,              // join, dns, time
  NoAddress,           // network
  ConnectFailed,       // https
  Certificate,         // https
  TlsFailed,           // https
  Redirect,            // https
  Unauthorized,        // mailbox: 401 or 403
  ServerError,         // mailbox: 5xx
  UnexpectedResponse,  // mailbox
};
const char* detailName(Detail detail);  // the result file's codes

// The reader's plain sentence for how the check ended (the result file carries the English text).
enum class Message : uint8_t {
  None,
  JoinFailed,
  NoAddress,
  DnsFailed,
  TimeFailed,
  CertificateFailed,
  MailboxUnreachable,
  AccessRejected,
  UnexpectedAnswer,
  Connected,
  Canceled,
  NotSetUp,
};
const char* messageText(Message message);

enum class Outcome : uint8_t { Running, Ok, Failed, Canceled, NotSetUp };
const char* outcomeName(Outcome outcome);  // "running", "ok", "failed", "canceled", "not_set_up"

// The driver's disconnect reason, as a join detail: 201 NO_AP_FOUND and 212 NO_AP_FOUND_IN_RSSI_THRESHOLD are
// not_found; 2 AUTH_EXPIRE, 15 4WAY_HANDSHAKE_TIMEOUT, 23 802_1X_AUTH_FAILED, 202 AUTH_FAIL and 204 HANDSHAKE_TIMEOUT
// are auth_failed; anything else is failed.
Detail joinDetail(int reason);

// ---- ports ----------------------------------------------------------------------------------------------------------

// What the reader will join, for the result file. Not configured: nothing to join (the file then has no "network").
struct NetworkInfo {
  bool configured = false;
  std::string source;  // "profile" or "saved"
  std::string name;
  std::string ssid;
  std::string auth;  // "open", "psk" or "eap-tls"
};

struct AddressInfo {
  char ip[16] = {};
  char gateway[16] = {};
  char dns[2][16] = {};
  uint8_t dnsCount = 0;
};

enum class JoinState : uint8_t { Pending, Joined, Failed };
enum class AddressState : uint8_t { None, Partial, Complete };

class NetworkPort {
 public:
  virtual ~NetworkPort() = default;
  virtual NetworkInfo describe() = 0;
  virtual void beginJoin() = 0;  // non-blocking
  // Joined: associated and authenticated. Failed: the driver gave up on this attempt.
  virtual JoinState joinState() = 0;
  // The driver's latest disconnect reason since beginJoin(), 0 when it gave none.
  virtual int disconnectReason() = 0;
  virtual int rssi() = 0;
  virtual int channel() = 0;
  // -1 when the join is not EAP-TLS (null in the file); else whether the server certificate's dates were checked.
  virtual int eapTimeChecked() = 0;
  virtual bool linkUp() = 0;
  // Complete once there is an IPv4 address, a gateway and a DNS server; Partial when only some are known.
  virtual AddressState address(AddressInfo& out) = 0;
  virtual void off() = 0;  // abandons a join and switches the radio off; safe any time, any number of times
};

class TimePort {
 public:
  virtual ~TimePort() = default;
  virtual uint32_t nowMs() = 0;  // monotonic
  // A fresh SNTP sync. Neither call waits; abortSync() is safe any time and keeps a permission a finished sync earned.
  virtual void beginSync() = 0;
  virtual method::SyncStatus syncStatus() = 0;
  virtual void abortSync() = 0;
  // After the sync failed: may the retained clock stand in (message_tls::useRetainedClock())?
  virtual bool useRetained() = 0;
};

// The mailbox request: method::HttpPort (name lookup, heap check, open, read, close) plus why an open failed.
class HttpPort : public method::HttpPort {
 public:
  virtual httpfail::Outcome outcome() = 0;
};

// Optional: stage timings for the serial log.
class Observer {
 public:
  virtual ~Observer() = default;
  virtual void stageEnded(StageId stage, Status status, Detail detail, uint32_t ms) = 0;
};

struct Ports {
  NetworkPort& network;
  TimePort& time;
  HttpPort& http;
  Observer* observer = nullptr;
};

struct Config {
  bool configured = false;  // the mailbox's address, device id and token are all set
  std::string deviceId;
  std::string host;  // the mailbox host, for the file and for a redirect without a host of its own
};

struct Limits {
  uint32_t joinMs = 20000;
  uint32_t joinEapMs = 30000;
  uint32_t networkMs = 10000;
  uint32_t dnsMs = 8000;
  uint32_t timeMs = 20000;
  uint32_t httpsMs = 15000;
  uint32_t mailboxMs = 10000;
  size_t chunkBytes = 2048;
};

// ---- what the check found -------------------------------------------------------------------------------------------

struct StageResult {
  Status status = Status::Pending;
  uint32_t ms = 0;
  Detail detail = Detail::None;
};

struct Report {
  StageResult stages[STAGE_COUNT];
  Outcome outcome = Outcome::Running;
  Message message = Message::None;
  NetworkInfo network;
  // join
  int reason = 0;
  bool linkKnown = false;
  int rssi = 0;
  int channel = 0;
  int eapTimeChecked = -1;
  // network
  AddressInfo address;
  // time
  clockprov::Source timeSource = clockprov::Source::None;
  // https: a redirect's Location host (the mailbox host when the Location named none)
  char locationHost[254] = {};
  // mailbox: the reply's status, 0 when none was read
  int httpStatus = 0;

  const StageResult& stage(StageId id) const { return stages[static_cast<size_t>(id)]; }
};

class Check {
  Ports ports_;
  Config config_;
  Limits limits_;
  Report report_;
  StageId stage_ = StageId::Join;
  bool running_ = false;
  bool finished_ = false;
  uint32_t stageStart_ = 0;
  uint32_t revision_ = 0;
  bool statusFailure_ = false;  // the request answered with a status the mailbox stage reports
  std::unique_ptr<uint8_t[]> chunk_;
  std::unique_ptr<char[]> reply_;
  size_t replyLength_ = 0;
  size_t replyFilled_ = 0;
  uint32_t joinDeadline_ = 0;

  StageResult& stage(StageId id) { return report_.stages[static_cast<size_t>(id)]; }
  bool deadlinePassed(uint32_t limitMs);
  void enter(StageId stage);
  void pass(StageId next);
  void fail(Detail detail);
  void finish(Outcome outcome, Message message);
  void teardown();
  void touch() { ++revision_; }

  void stepJoin();
  void stepNetwork();
  void stepDns();
  void stepTime();
  void stepHttps();
  void stepMailbox();
  // The request itself. True when the mailbox stage takes over: the body is to be read, or the answer reported.
  bool openRequest();
  void failFromStatus();
  void readReply();

 public:
  Check(const Ports& ports, Config config, Limits limits = {});
  ~Check();
  Check(const Check&) = delete;
  Check& operator=(const Check&) = delete;

  void start();
  void step();
  void cancel();

  bool running() const { return running_; }
  bool finished() const { return finished_; }
  StageId stage() const { return stage_; }
  const Report& report() const { return report_; }
  // Changes whenever something worth repainting changed.
  uint32_t revision() const { return revision_; }
};

// ---- the result file ------------------------------------------------------------------------------------------------

constexpr char RESULT_FOLDER[] = "/Lamina";
constexpr char RESULT_PATH[] = "/Lamina/connection-check.json";
constexpr char RESULT_TMP_PATH[] = "/Lamina/connection-check.json.tmp";

// Everything in the file that the check does not find itself. Nothing secret is passed in, so nothing secret can
// come out.
struct ResultContext {
  int64_t finishedAt = 0;  // UTC seconds; 0 writes null (no trusted time)
  clockprov::Source clock = clockprov::Source::None;
  uint32_t bootMs = 0;
  std::string readerUuid;
  std::string model;
  std::string firmware;
  std::string stationMac;
  std::string mailboxHost;  // empty writes null (not set up)
};

// /Lamina/connection-check.json, schema 1, with a trailing newline.
std::string resultJson(const ResultContext& context, const Report& report);

// "AA:BB:CC:DD:EE:FF".
std::string macText(const uint8_t mac[6]);

}  // namespace conncheck
