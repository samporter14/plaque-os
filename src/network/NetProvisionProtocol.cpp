#include "NetProvisionProtocol.h"

#include <atomic>
#include <cstring>

namespace netprov {
namespace {
constexpr size_t PREFIX_BYTES = sizeof(PREFIX) - 1;
std::atomic<bool> serialClaimed{false};

bool parseSize(const std::string_view text, size_t& out) {
  if (text.empty() || text.size() > 7) return false;
  out = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    out = out * 10 + static_cast<size_t>(c - '0');
  }
  return true;
}
}  // namespace

void claimSerialInput(const bool claimed) { serialClaimed.store(claimed); }
bool serialInputClaimed() { return serialClaimed.load(); }

Session::Session(Host& host, uint8_t* buffer, const size_t capacity)
    : host_(host), buffer_(buffer), capacity_(capacity) {}

Session::~Session() { close(); }

void Session::feed(const uint8_t* data, const size_t size, const uint32_t nowMs) {
  for (size_t i = 0; i < size; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c == '\n') {
      handleLine(nowMs);
      continue;
    }
    if (lineLength_ < MAX_LINE_BYTES) {
      line_[lineLength_++] = c;
    } else {
      lineOverflow_ = true;  // the start is kept: it tells a protocol line from noise
    }
  }
}

void Session::tick(const uint32_t nowMs, const bool linkUp) {
  if (linkUp) {
    linkDownSince_ = 0;
  } else if (linkDownSince_ == 0) {
    linkDownSince_ = nowMs ? nowMs : 1;
  }
  const bool linkGone = linkDownSince_ != 0 && static_cast<uint32_t>(nowMs - linkDownSince_) >= LINK_GRACE_MS;
  if (state_ == State::AwaitingInstall || state_ == State::AwaitingRemove) {
    if (linkGone) {
      endQuestion(Host::Ended::LinkLost);
    } else if (static_cast<uint32_t>(nowMs - questionSince_) >= QUESTION_TIMEOUT_MS) {
      endQuestion(Host::Ended::TimedOut);
    }
    return;
  }
  if (state_ != State::Receiving) return;
  if (linkGone) {
    dropTransfer();  // nobody is sending any more
    return;
  }
  if (static_cast<uint32_t>(nowMs - lastData_) < STALL_MS) return;
  dropTransfer();
  stalled_ = true;
}

void Session::questionTouched(const uint32_t nowMs) {
  if (state_ == State::AwaitingInstall || state_ == State::AwaitingRemove) questionSince_ = nowMs;
}

// Back, in the person's absence. A timeout is answered (the host may still be listening); a lost link has nobody to
// hear.
void Session::endQuestion(const Host::Ended why) {
  state_ = State::Idle;
  if (why == Host::Ended::TimedOut) replyError(netprofile::Error::Declined);
  host_.questionEnded(why);
}

void Session::handleLine(const uint32_t nowMs) {
  std::string_view line(line_, lineLength_);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  const bool overflow = lineOverflow_;
  if (line.substr(0, PREFIX_BYTES) == std::string_view(PREFIX, PREFIX_BYTES)) {
    lastTraffic_ = nowMs ? nowMs : 1;
    if (overflow) {
      if (state_ == State::Receiving) dropTransfer();
      if (state_ == State::Idle)
        replyError(netprofile::Error::BadField);
      else
        replyError(netprofile::Error::Busy);
    } else if (stalled_) {
      stalled_ = false;
      replyError(netprofile::Error::Busy);
    } else {
      const std::string_view rest = line.substr(PREFIX_BYTES);
      const size_t space = rest.find(' ');
      handleCommand(rest.substr(0, space),
                    space == std::string_view::npos ? std::string_view() : rest.substr(space + 1), nowMs);
    }
  }
  // A DATA line is part of a secret: nothing of it stays in the line buffer.
  netprofile::wipe(line_, sizeof(line_));
  lineLength_ = 0;
  lineOverflow_ = false;
}

void Session::handleCommand(const std::string_view command, const std::string_view argument, const uint32_t nowMs) {
  if (state_ == State::AwaitingInstall || state_ == State::AwaitingRemove) {
    replyError(netprofile::Error::Busy);
    return;
  }
  if (state_ == State::Receiving) {
    if (command == "DATA") {
      data(argument, nowMs);
    } else if (command == "END" && argument.empty()) {
      end(nowMs);
    } else {
      dropTransfer();  // anything else in the middle of a transfer ends it
      replyError(netprofile::Error::Busy);
    }
    return;
  }
  std::string content;
  if (command == "HELLO") {
    reply("READY " + std::to_string(netprofile::MAX_BUNDLE_BYTES));
  } else if (command == "INFO") {
    const bool found = host_.readerInfo(content);
    replyFile("INFO", found, content);
  } else if (command == "PROFILE") {
    const bool found = host_.profileSummary(content);
    replyFile("PROFILE", found, content);
  } else if (command == "RESULT") {
    const bool found = host_.checkResult(content);
    replyFile("RESULT", found, content);
  } else if (command == "PUT") {
    beginTransfer(argument, nowMs);
  } else if (command == "REMOVE") {
    if (host_.removeArrived()) {
      state_ = State::AwaitingRemove;
      questionSince_ = nowMs;
    } else {
      reply("OK removed");
    }
  } else if (command == "DATA" || command == "END") {
    // The rest of a transfer that was already answered (an error or a drop): nothing more to say.
  } else {
    replyError(netprofile::Error::BadField);
  }
}

void Session::beginTransfer(const std::string_view argument, const uint32_t nowMs) {
  const size_t space = argument.find(' ');
  size_t bytes = 0;
  if (space == std::string_view::npos || !parseSize(argument.substr(0, space), bytes) || bytes == 0 ||
      !netcodec::fromHex64(argument.substr(space + 1), sha_)) {
    replyError(netprofile::Error::BadField);
    return;
  }
  if (bytes > netprofile::MAX_BUNDLE_BYTES || bytes > capacity_) {
    replyError(netprofile::Error::TooLarge);
    return;
  }
  decoder_.reset();
  expected_ = bytes;
  received_ = 0;
  lastData_ = nowMs;
  state_ = State::Receiving;
}

void Session::data(const std::string_view payload, const uint32_t nowMs) {
  if (payload.empty() || payload.size() > MAX_DATA_CHARS) {
    dropTransfer();
    replyError(netprofile::Error::BadField);
    return;
  }
  // The output is bounded by the announced size: more data than announced fails here, like a bad character.
  if (!decoder_.feed(payload, buffer_, expected_, received_, false)) {
    dropTransfer();
    replyError(netprofile::Error::Checksum);
    return;
  }
  decoder_.endPiece();
  lastData_ = nowMs;
}

void Session::end(const uint32_t nowMs) {
  uint8_t digest[32];
  if (!decoder_.complete() || received_ != expected_ || !netcodec::sha256(buffer_, received_, digest) ||
      std::memcmp(digest, sha_, sizeof(digest)) != 0) {
    dropTransfer();
    replyError(netprofile::Error::Checksum);
    return;
  }
  const netprofile::Error result = host_.bundleArrived(buffer_, received_);
  dropTransfer();  // wipes the bundle: the host keeps what it validated
  if (result == netprofile::Error::None) {
    reply("CONFIRM");
    state_ = State::AwaitingInstall;
    questionSince_ = nowMs;
  } else {
    replyError(result);
  }
}

void Session::installDecided(const netprofile::Error result) {
  if (state_ != State::AwaitingInstall) return;
  state_ = State::Idle;
  if (result == netprofile::Error::None) {
    reply("OK installed");
  } else {
    replyError(result);
  }
}

void Session::removeDecided(const netprofile::Error result) {
  if (state_ != State::AwaitingRemove) return;
  state_ = State::Idle;
  if (result == netprofile::Error::None) {
    reply("OK removed");
  } else {
    replyError(result);
  }
}

void Session::close() {
  if (state_ == State::AwaitingInstall || state_ == State::AwaitingRemove) replyError(netprofile::Error::Declined);
  dropTransfer();
  stalled_ = false;
  netprofile::wipe(line_, sizeof(line_));
  lineLength_ = 0;
  lineOverflow_ = false;
}

void Session::reply(const std::string_view text) {
  std::string line(PREFIX);
  line.append(text.data(), text.size());
  host_.sendLine(line);
}

void Session::replyError(const netprofile::Error error) {
  ++errorCount_;
  lastError_ = error;
  reply(std::string("ERR ") + netprofile::errorCode(error));
}

void Session::replyFile(const std::string_view word, const bool found, const std::string& content) {
  std::string text(word);
  if (!found || content.empty() || content.size() > MAX_REPLY_FILE_BYTES) {
    text += " none";
  } else {
    text += ' ';
    text += netcodec::base64Encode(reinterpret_cast<const uint8_t*>(content.data()), content.size());
  }
  reply(text);
}

void Session::dropTransfer() {
  if (buffer_) netprofile::wipe(buffer_, capacity_);
  decoder_.reset();
  expected_ = 0;
  received_ = 0;
  state_ = State::Idle;
}

}  // namespace netprov
