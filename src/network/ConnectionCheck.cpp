#include "ConnectionCheck.h"

#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include "ambient/MethodProtocol.h"

namespace conncheck {

const char* stageName(const StageId stage) {
  switch (stage) {
    case StageId::Join:
      return "join";
    case StageId::Network:
      return "network";
    case StageId::Dns:
      return "dns";
    case StageId::Time:
      return "time";
    case StageId::Https:
      return "https";
    case StageId::Mailbox:
      return "mailbox";
  }
  return "";
}

const char* statusName(const Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Failed:
      return "failed";
    case Status::Canceled:
      return "canceled";
    case Status::Pending:
    case Status::Running:
    case Status::NotRun:
      break;
  }
  return "not_run";
}

const char* detailName(const Detail detail) {
  switch (detail) {
    case Detail::None:
      return "";
    case Detail::NothingConfigured:
      return "nothing_configured";
    case Detail::NotFound:
      return "not_found";
    case Detail::AuthFailed:
      return "auth_failed";
    case Detail::Timeout:
      return "timeout";
    case Detail::Failed:
      return "failed";
    case Detail::NoAddress:
      return "no_address";
    case Detail::ConnectFailed:
      return "connect_failed";
    case Detail::Certificate:
      return "certificate";
    case Detail::TlsFailed:
      return "tls_failed";
    case Detail::Redirect:
      return "redirect";
    case Detail::Unauthorized:
      return "unauthorized";
    case Detail::ServerError:
      return "server_error";
    case Detail::UnexpectedResponse:
      return "unexpected_response";
  }
  return "";
}

const char* messageText(const Message message) {
  switch (message) {
    case Message::None:
      return "";
    case Message::JoinFailed:
      return "Couldn't join this network.";
    case Message::NoAddress:
      return "Wi-Fi joined. Couldn't get a network address.";
    case Message::DnsFailed:
      return "Wi-Fi joined. Couldn't resolve the mailbox.";
    case Message::TimeFailed:
      return "Couldn't synchronize the clock.";
    case Message::CertificateFailed:
      return "Couldn't verify the server certificate.";
    case Message::MailboxUnreachable:
      return "Couldn't reach the mailbox.";
    case Message::AccessRejected:
      return "Mailbox access was rejected.";
    case Message::UnexpectedAnswer:
      return "The mailbox answered, but not as expected.";
    case Message::Connected:
      return "Connected to the mailbox.";
    case Message::Canceled:
      return "Check canceled.";
    case Message::NotSetUp:
      return "The mailbox isn't set up. Add it under Settings \xE2\x80\xBA System \xE2\x80\xBA Messages.";
  }
  return "";
}

const char* outcomeName(const Outcome outcome) {
  switch (outcome) {
    case Outcome::Running:
      return "running";
    case Outcome::Ok:
      return "ok";
    case Outcome::Failed:
      return "failed";
    case Outcome::Canceled:
      return "canceled";
    case Outcome::NotSetUp:
      return "not_set_up";
  }
  return "";
}

Detail joinDetail(const int reason) {
  switch (reason) {
    case 201:  // NO_AP_FOUND
    case 212:  // NO_AP_FOUND_IN_RSSI_THRESHOLD
      return Detail::NotFound;
    case 2:    // AUTH_EXPIRE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 23:   // 802_1X_AUTH_FAILED
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
      return Detail::AuthFailed;
    default:
      return Detail::Failed;
  }
}

namespace {
Message failureMessage(const StageId stage, const Detail detail) {
  switch (stage) {
    case StageId::Join:
      return Message::JoinFailed;
    case StageId::Network:
      return Message::NoAddress;
    case StageId::Dns:
      return Message::DnsFailed;
    case StageId::Time:
      return Message::TimeFailed;
    case StageId::Https:
      return detail == Detail::Certificate ? Message::CertificateFailed : Message::MailboxUnreachable;
    case StageId::Mailbox:
      return detail == Detail::Unauthorized ? Message::AccessRejected : Message::UnexpectedAnswer;
  }
  return Message::None;
}
}  // namespace

// ---- the check ------------------------------------------------------------------------------------------------------

Check::Check(const Ports& ports, Config config, Limits limits)
    : ports_(ports), config_(std::move(config)), limits_(limits) {}

Check::~Check() { cancel(); }

bool Check::deadlinePassed(const uint32_t limitMs) {
  return static_cast<uint32_t>(ports_.time.nowMs() - stageStart_) >= limitMs;
}

void Check::start() {
  if (running_) return;
  report_ = Report{};
  finished_ = false;
  stage_ = StageId::Join;
  touch();
  report_.network = ports_.network.describe();
  if (!config_.configured) {
    finish(Outcome::NotSetUp, Message::NotSetUp);
    return;
  }
  running_ = true;
  enter(StageId::Join);
}

void Check::step() {
  if (!running_) return;
  switch (stage_) {
    case StageId::Join:
      stepJoin();
      break;
    case StageId::Network:
      stepNetwork();
      break;
    case StageId::Dns:
      stepDns();
      break;
    case StageId::Time:
      stepTime();
      break;
    case StageId::Https:
      stepHttps();
      break;
    case StageId::Mailbox:
      stepMailbox();
      break;
  }
}

void Check::cancel() {
  if (!running_) return;
  StageResult& current = stage(stage_);
  current.status = Status::Canceled;
  current.ms = static_cast<uint32_t>(ports_.time.nowMs() - stageStart_);
  if (ports_.observer) ports_.observer->stageEnded(stage_, Status::Canceled, Detail::None, current.ms);
  finish(Outcome::Canceled, Message::Canceled);
}

void Check::enter(const StageId next) {
  stage_ = next;
  stageStart_ = ports_.time.nowMs();
  stage(next).status = Status::Running;
  touch();
  switch (next) {
    case StageId::Join:
      if (!report_.network.configured) {
        fail(Detail::NothingConfigured);
        return;
      }
      joinDeadline_ = report_.network.auth == "eap-tls" ? limits_.joinEapMs : limits_.joinMs;
      ports_.network.beginJoin();
      break;
    case StageId::Dns:
      ports_.http.beginResolve();
      break;
    case StageId::Time:
      ports_.time.beginSync();
      break;
    case StageId::Network:
    case StageId::Https:
    case StageId::Mailbox:
      break;
  }
}

// The current stage succeeded: on to the next, or the whole check succeeded.
void Check::pass(const StageId next) {
  StageResult& current = stage(stage_);
  current.status = Status::Ok;
  current.ms = static_cast<uint32_t>(ports_.time.nowMs() - stageStart_);
  if (ports_.observer) ports_.observer->stageEnded(stage_, Status::Ok, Detail::None, current.ms);
  if (stage_ == StageId::Mailbox) {
    finish(Outcome::Ok, Message::Connected);
    return;
  }
  enter(next);
}

void Check::fail(const Detail detail) {
  StageResult& current = stage(stage_);
  current.status = Status::Failed;
  current.detail = detail;
  current.ms = static_cast<uint32_t>(ports_.time.nowMs() - stageStart_);
  if (ports_.observer) ports_.observer->stageEnded(stage_, Status::Failed, detail, current.ms);
  finish(Outcome::Failed, failureMessage(stage_, detail));
}

void Check::finish(const Outcome outcome, const Message message) {
  teardown();
  for (auto& result : report_.stages)
    if (result.status == Status::Pending || result.status == Status::Running) result.status = Status::NotRun;
  report_.outcome = outcome;
  report_.message = message;
  running_ = false;
  finished_ = true;
  touch();
}

void Check::teardown() {
  // The connection first, while the radio is still up; then the clock sync and its power-save guard; then the radio.
  ports_.http.abortResolve();
  ports_.http.close();
  ports_.time.abortSync();
  ports_.network.off();
  chunk_.reset();
  reply_.reset();
  replyLength_ = replyFilled_ = 0;
  statusFailure_ = false;
}

// ---- stages ---------------------------------------------------------------------------------------------------------

void Check::stepJoin() {
  const JoinState state = ports_.network.joinState();
  const int reason = ports_.network.disconnectReason();
  if (reason != 0) report_.reason = reason;
  if (state == JoinState::Joined) {
    // A reason from an attempt the driver then retried and won is not a failure.
    report_.reason = 0;
    report_.linkKnown = true;
    report_.rssi = ports_.network.rssi();
    report_.channel = ports_.network.channel();
    report_.eapTimeChecked = ports_.network.eapTimeChecked();
    pass(StageId::Network);
    return;
  }
  const bool expired = deadlinePassed(joinDeadline_);
  if (state != JoinState::Failed && !expired) return;
  report_.eapTimeChecked = ports_.network.eapTimeChecked();
  if (report_.reason != 0) {
    fail(joinDetail(report_.reason));
  } else {
    fail(state == JoinState::Failed ? Detail::Failed : Detail::Timeout);
  }
}

void Check::stepNetwork() {
  AddressInfo info;
  const AddressState state = ports_.network.address(info);
  if (state == AddressState::Complete) {
    report_.address = info;
    pass(StageId::Dns);
    return;
  }
  if (!ports_.network.linkUp()) {
    fail(Detail::NoAddress);
    return;
  }
  if (deadlinePassed(limits_.networkMs)) fail(state == AddressState::Partial ? Detail::NoAddress : Detail::Timeout);
}

void Check::stepDns() {
  switch (ports_.http.resolveStatus()) {
    case method::ResolveStatus::Done:
      pass(StageId::Time);
      return;
    case method::ResolveStatus::Failed:
      fail(Detail::Failed);
      return;
    case method::ResolveStatus::Pending:
      if (deadlinePassed(limits_.dnsMs)) fail(Detail::Timeout);
      return;
  }
}

void Check::stepTime() {
  const method::SyncStatus status = ports_.time.syncStatus();
  if (status == method::SyncStatus::Synced) {
    report_.timeSource = clockprov::Source::Sntp;
    pass(StageId::Https);
    return;
  }
  const bool expired = deadlinePassed(limits_.timeMs);
  if (status != method::SyncStatus::Failed && !expired) return;
  // SNTP failed: the retained clock may stand in (section 7), and the check says which it was.
  ports_.time.abortSync();
  if (ports_.time.useRetained()) {
    report_.timeSource = clockprov::Source::Retained;
    pass(StageId::Https);
    return;
  }
  fail(status == method::SyncStatus::Failed ? Detail::Failed : Detail::Timeout);
}

void Check::stepHttps() {
  if (deadlinePassed(limits_.httpsMs)) {
    fail(Detail::Timeout);
    return;
  }
  switch (ports_.http.ready()) {
    case method::HttpReady::Wait:
      return;  // the render task has the font caches; ask again on the next step
    case method::HttpReady::NoMemory:
      fail(Detail::TlsFailed);
      return;
    case method::HttpReady::Yes:
      break;
  }
  if (!openRequest()) return;
  pass(StageId::Mailbox);
  if (running_ && statusFailure_) failFromStatus();
}

bool Check::openRequest() {
  const std::string route = "/api/device/" + config_.deviceId + "/books";
  const method::HttpOpen opened = ports_.http.open(route.c_str(), method::MAX_LIST_BYTES);
  const httpfail::Outcome outcome = ports_.http.outcome();
  report_.httpStatus = outcome.status;
  if (opened == method::HttpOpen::Ok) return true;
  const bool expired = deadlinePassed(limits_.httpsMs);
  switch (outcome.failure) {
    case httpfail::Failure::Status:
    case httpfail::Failure::BadLength:
      // TLS held and the mailbox answered: the answer is the mailbox stage's to report.
      statusFailure_ = true;
      return true;
    case httpfail::Failure::Redirect:
      std::snprintf(report_.locationHost, sizeof(report_.locationHost), "%s",
                    outcome.locationHost[0] ? outcome.locationHost : config_.host.c_str());
      fail(Detail::Redirect);
      return false;
    case httpfail::Failure::Certificate:
      fail(Detail::Certificate);
      return false;
    case httpfail::Failure::Timeout:
      fail(Detail::Timeout);
      return false;
    case httpfail::Failure::Connect:
      fail(expired ? Detail::Timeout : Detail::ConnectFailed);
      return false;
    case httpfail::Failure::Tls:
    case httpfail::Failure::Preflight:
    case httpfail::Failure::None:
      break;
  }
  fail(expired ? Detail::Timeout : Detail::TlsFailed);
  return false;
}

void Check::failFromStatus() {
  switch (httpfail::classifyStatus(report_.httpStatus)) {
    case httpfail::StatusClass::Unauthorized:
      fail(Detail::Unauthorized);
      return;
    case httpfail::StatusClass::NotFound:
      fail(Detail::NotFound);
      return;
    case httpfail::StatusClass::ServerError:
      fail(Detail::ServerError);
      return;
    case httpfail::StatusClass::Ok:
    case httpfail::StatusClass::Gone:
    case httpfail::StatusClass::Redirect:
    case httpfail::StatusClass::Other:
      break;
  }
  fail(Detail::UnexpectedResponse);
}

void Check::stepMailbox() {
  if (statusFailure_) {
    failFromStatus();
    return;
  }
  if (deadlinePassed(limits_.mailboxMs)) {
    fail(Detail::Timeout);
    return;
  }
  readReply();
}

void Check::readReply() {
  if (!reply_) {
    const int64_t length = ports_.http.contentLength();
    if (length <= 0 || length > static_cast<int64_t>(method::MAX_LIST_BYTES)) {
      fail(Detail::UnexpectedResponse);
      return;
    }
    // Sized once from the announced length; a failed allocation is a result, not an abort.
    replyLength_ = static_cast<size_t>(length);
    replyFilled_ = 0;
    reply_ = makeUniqueNoThrow<char[]>(replyLength_);
    chunk_ = makeUniqueNoThrow<uint8_t[]>(limits_.chunkBytes);
    if (!reply_ || !chunk_) {
      fail(Detail::UnexpectedResponse);
      return;
    }
  }
  size_t got = 0;
  switch (ports_.http.read(chunk_.get(), limits_.chunkBytes, got)) {
    case method::HttpRead::Data:
      if (got > replyLength_ - replyFilled_) {
        fail(Detail::UnexpectedResponse);
        return;
      }
      std::memcpy(reply_.get() + replyFilled_, chunk_.get(), got);
      replyFilled_ += got;
      return;
    case method::HttpRead::Idle:
      return;
    case method::HttpRead::Failed:
      fail(Detail::UnexpectedResponse);
      return;
    case method::HttpRead::Done:
      break;
  }
  ports_.http.close();
  std::vector<method::BookMetadata> books;
  uint32_t newest = 0;
  size_t skipped = 0;
  // Only whether the list parses matters here; what it lists (titles included) goes nowhere.
  const bool valid = replyFilled_ == replyLength_ && method::parseBooks(std::string_view(reply_.get(), replyFilled_),
                                                                        config_.deviceId, books, newest, skipped);
  if (!valid) {
    fail(Detail::UnexpectedResponse);
    return;
  }
  pass(StageId::Mailbox);
}

// ---- the result file ------------------------------------------------------------------------------------------------

namespace {
// Length of the valid UTF-8 sequence at text[i], 0 when the bytes there are not one.
size_t utf8Length(const std::string_view text, const size_t i) {
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(text[k]); };
  const auto cont = [&](size_t k) { return k < text.size() && (byte(k) & 0xC0) == 0x80; };
  const unsigned char c = byte(i);
  if (c >= 0xC2 && c <= 0xDF) return cont(i + 1) ? 2 : 0;
  if (c >= 0xE0 && c <= 0xEF) {
    if (!cont(i + 1) || !cont(i + 2)) return 0;
    if (c == 0xE0 && byte(i + 1) < 0xA0) return 0;  // overlong
    if (c == 0xED && byte(i + 1) > 0x9F) return 0;  // surrogate
    return 3;
  }
  if (c >= 0xF0 && c <= 0xF4) {
    if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) return 0;
    if (c == 0xF0 && byte(i + 1) < 0x90) return 0;  // overlong
    if (c == 0xF4 && byte(i + 1) > 0x8F) return 0;  // beyond U+10FFFF
    return 4;
  }
  return 0;
}

// A JSON string. Bytes that are not UTF-8 (a saved network name can be anything) become U+FFFD.
void appendString(std::string& out, const std::string_view text) {
  out += '"';
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
      ++i;
    } else if (c < 0x20) {
      char escaped[8];
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
      out += escaped;
      ++i;
    } else if (c < 0x80) {
      out += static_cast<char>(c);
      ++i;
    } else if (const size_t length = utf8Length(text, i); length > 0) {
      out.append(text.substr(i, length));
      i += length;
    } else {
      out += "\xEF\xBF\xBD";
      ++i;
    }
  }
  out += '"';
}

void appendStringOrNull(std::string& out, const std::string_view text) {
  if (text.empty()) {
    out += "null";
  } else {
    appendString(out, text);
  }
}

void appendKey(std::string& out, const char* key) {
  out += ",\"";
  out += key;
  out += "\":";
}

void appendNumber(std::string& out, const char* key, const long long value) {
  appendKey(out, key);
  out += std::to_string(value);
}

void appendStage(std::string& out, const StageId id, const Report& report) {
  const StageResult& result = report.stage(id);
  out += "{\"id\":\"";
  out += stageName(id);
  out += "\",\"status\":\"";
  out += statusName(result.status);
  out += '"';
  if (result.status == Status::NotRun || result.status == Status::Pending || result.status == Status::Running) {
    out += '}';
    return;
  }
  appendNumber(out, "ms", result.ms);
  if (result.detail != Detail::None) {
    appendKey(out, "detail");
    appendString(out, detailName(result.detail));
  }
  switch (id) {
    case StageId::Join:
      if (result.status == Status::Ok && report.linkKnown) {
        appendNumber(out, "rssi", report.rssi);
        appendNumber(out, "channel", report.channel);
      }
      if (result.status == Status::Failed && report.reason != 0) appendNumber(out, "reason", report.reason);
      appendKey(out, "eap_time_checked");
      out += report.eapTimeChecked < 0 ? "null" : (report.eapTimeChecked ? "true" : "false");
      break;
    case StageId::Network:
      if (result.status == Status::Ok) {
        appendKey(out, "ip");
        appendString(out, report.address.ip);
        appendKey(out, "gateway");
        appendString(out, report.address.gateway);
        appendKey(out, "dns");
        out += '[';
        for (uint8_t i = 0; i < report.address.dnsCount && i < 2; ++i) {
          if (i) out += ',';
          appendString(out, report.address.dns[i]);
        }
        out += ']';
      }
      break;
    case StageId::Https:
      if (result.detail == Detail::Redirect) {
        appendKey(out, "location_host");
        appendStringOrNull(out, report.locationHost);
      }
      break;
    case StageId::Mailbox:
      if (report.httpStatus > 0) appendNumber(out, "http_status", report.httpStatus);
      break;
    case StageId::Dns:
    case StageId::Time:
      break;
  }
  out += '}';
}
}  // namespace

std::string resultJson(const ResultContext& context, const Report& report) {
  std::string out;
  out.reserve(1024);
  out += "{\"schema\":1,\"kind\":\"plaque-connection-check\"";
  appendKey(out, "finished_at");
  out += context.finishedAt > 0 ? std::to_string(context.finishedAt) : std::string("null");
  appendKey(out, "clock");
  appendString(out, clockprov::sourceName(context.clock));
  appendNumber(out, "boot_ms", context.bootMs);
  appendKey(out, "reader_uuid");
  appendStringOrNull(out, context.readerUuid);
  appendKey(out, "model");
  appendStringOrNull(out, context.model);
  appendKey(out, "firmware");
  appendStringOrNull(out, context.firmware);
  appendKey(out, "station_mac");
  appendStringOrNull(out, context.stationMac);
  if (report.network.configured) {
    appendKey(out, "network");
    out += "{\"source\":";
    appendString(out, report.network.source);
    out += ",\"name\":";
    appendString(out, report.network.name);
    out += ",\"ssid\":";
    appendString(out, report.network.ssid);
    out += ",\"auth\":";
    appendString(out, report.network.auth);
    out += '}';
  }
  appendKey(out, "mailbox_host");
  appendStringOrNull(out, context.mailboxHost);
  appendKey(out, "result");
  appendString(out, outcomeName(report.outcome));
  appendKey(out, "message");
  appendString(out, messageText(report.message));
  appendKey(out, "stages");
  out += '[';
  for (size_t i = 0; i < STAGE_COUNT; ++i) {
    if (i) out += ',';
    appendStage(out, static_cast<StageId>(i), report);
  }
  out += "]}\n";
  return out;
}

std::string macText(const uint8_t mac[6]) {
  char text[18];
  std::snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return text;
}

}  // namespace conncheck
