#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "NetCodec.h"
#include "NetworkProfile.h"

// The USB setup link (SPEC-UNIVERSITY-WIFI.md section 5): an ASCII line protocol over the reader's USB serial port,
// shared with the log. Every protocol line starts with "PLAQUE-NET/1 "; every other line is noise and ignored.
//
//   HELLO                      -> READY 16384
//   INFO | PROFILE | RESULT    -> INFO <base64> | PROFILE <base64> | RESULT <base64> (or "<WORD> none")
//   PUT <bytes> <sha256-hex>, DATA <base64, at most 256 characters>..., END
//                              -> CONFIRM while the person decides, then OK installed or ERR <code>
//   REMOVE                     -> OK removed or ERR declined, after the person decides
//
// The DATA payloads are one base64 stream, split anywhere; a padded group may also end a DATA line, so a sender that
// encodes each line on its own is understood too. The transfer must arrive whole, with the announced length and
// SHA-256 (ERR checksum otherwise), and within MAX_BUNDLE_BYTES (ERR too_large). A transfer with no line for
// STALL_MS is dropped, and the next protocol line is answered ERR busy. While the person is deciding, every protocol
// line is answered ERR busy. The question does not wait for ever: after QUESTION_TIMEOUT_MS with no answer (each press
// or touch on it starts the time again) it is taken as Back (ERR declined, the bundle forgotten), and so it is when the
// link has been gone for LINK_GRACE_MS (the USB cable out, the host away) with nobody to hear the answer. Nothing
// received is ever echoed: replies carry codes and the reader's own files only.
//
// Pure: the reader's side (the serial port, the screen, the store) is the Host. test/network_profile covers it.
namespace netprov {

constexpr char PREFIX[] = "PLAQUE-NET/1 ";
constexpr size_t MAX_LINE_BYTES = 400;  // a DATA line is 13 + 5 + 256 characters
constexpr size_t MAX_DATA_CHARS = 256;
constexpr uint32_t STALL_MS = 10000;
constexpr uint32_t QUESTION_TIMEOUT_MS = 120000;  // an install or remove question nobody answers is declined
constexpr uint32_t LINK_GRACE_MS = 3000;          // the link flaps for a moment on a healthy cable; longer is gone
constexpr size_t MAX_REPLY_FILE_BYTES = 8192;

// The most bytes one legal transfer puts on the port: the PUT line, one DATA line for every 256 base64 characters of
// the largest bundle (16 384 bytes make 21 848 characters, 86 lines) and the END line, each with its newline (and a
// carriage return, for a sender that writes CRLF), and the prefix on every line.
constexpr size_t MAX_BASE64_CHARS = ((netprofile::MAX_BUNDLE_BYTES + 2) / 3) * 4;
constexpr size_t MAX_DATA_LINES = (MAX_BASE64_CHARS + MAX_DATA_CHARS - 1) / MAX_DATA_CHARS;
constexpr size_t MAX_TRANSFER_WIRE_BYTES = (sizeof(PREFIX) - 1 + 4 + 5 + 1 + 64 + 2) +  // PUT <bytes> <sha256-hex>
                                           MAX_DATA_LINES * (sizeof(PREFIX) - 1 + 5 + 2) +
                                           MAX_BASE64_CHARS +             // DATA <base64>
                                           (sizeof(PREFIX) - 1 + 3 + 2);  // END
// The USB receive queue while Connect to Mac listens. The Mac sends its DATA lines without pacing, and the Arduino
// core's USB Serial/JTAG interrupt (HWCDC.cpp, hw_cdc_isr_handler) always empties the hardware FIFO into that queue,
// byte by byte, and throws away what does not fit: there is no backpressure (the endpoint is never left unread, so the
// host is never told to wait). The only safe size is one that holds a whole legal transfer, whatever the loop task is
// doing meanwhile (waiting out a panel refresh for the render lock, say). It is internal RAM, held only while the
// screen is open.
constexpr size_t RX_QUEUE_BYTES = ((MAX_TRANSFER_WIRE_BYTES + 1023) / 1024) * 1024;

class Host {
 public:
  virtual ~Host() = default;
  // One whole protocol line, prefix included, without the newline.
  virtual void sendLine(std::string_view line) = 0;
  // The reader's files for INFO, PROFILE and RESULT. False when there is none.
  virtual bool readerInfo(std::string& out) = 0;
  virtual bool profileSummary(std::string& out) = 0;
  virtual bool checkResult(std::string& out) = 0;
  // A bundle that arrived whole. Error::None means it validated and the person is now being asked (the session
  // sends CONFIRM and waits for installDecided()); anything else is sent as the error. The bytes are wiped as soon as
  // this returns: the host keeps what it validated.
  virtual netprofile::Error bundleArrived(const uint8_t* data, size_t size) = 0;
  // REMOVE. True when the person is now being asked (the session waits for removeDecided()); false when nothing is
  // installed, which is answered OK removed at once.
  virtual bool removeArrived() = 0;
  // The question was dropped without the person's answer: nothing was installed or removed, and the host takes the
  // dialog down, forgets what it validated and says why on the screen.
  enum class Ended : uint8_t { TimedOut, LinkLost };
  virtual void questionEnded(Ended why) = 0;
};

class Session {
 public:
  enum class State : uint8_t { Idle, Receiving, AwaitingInstall, AwaitingRemove };

  // `buffer` receives the bundle; it is the caller's (allocated once, at least MAX_BUNDLE_BYTES) and is wiped after
  // every transfer.
  Session(Host& host, uint8_t* buffer, size_t capacity);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Raw bytes from the port. Complete lines are handled as they arrive.
  void feed(const uint8_t* data, size_t size, uint32_t nowMs);
  // Drops a stalled transfer, and declines a question that nobody answered in time or whose link is gone. Call every
  // pass; `linkUp` is whether the host end of the link is there (the USB cable is plugged in).
  void tick(uint32_t nowMs, bool linkUp = true);
  // The person is working the question (a key or a touch on it): the time to answer starts again.
  void questionTouched(uint32_t nowMs);
  // The person's answer: Error::None (installed), Declined, or the store's error.
  void installDecided(netprofile::Error result);
  // Error::None (removed), Declined, or the store's error.
  void removeDecided(netprofile::Error result);
  // Abandons everything (the screen is closing): a pending question is answered ERR declined, buffers are wiped.
  void close();

  State state() const { return state_; }
  size_t received() const { return received_; }
  size_t expected() const { return expected_; }
  // Something protocol-shaped arrived at this time (for keeping the reader awake), or 0.
  uint32_t lastTrafficMs() const { return lastTraffic_; }
  // The errors answered so far, and the latest (for the screen).
  uint32_t errorCount() const { return errorCount_; }
  netprofile::Error lastError() const { return lastError_; }

 private:
  Host& host_;
  uint8_t* buffer_;
  size_t capacity_;
  State state_ = State::Idle;
  char line_[MAX_LINE_BYTES + 1] = {};
  size_t lineLength_ = 0;
  bool lineOverflow_ = false;
  size_t expected_ = 0;
  size_t received_ = 0;
  uint8_t sha_[32] = {};
  netcodec::Base64Decoder decoder_;
  uint32_t lastData_ = 0;
  uint32_t lastTraffic_ = 0;
  uint32_t questionSince_ = 0;
  uint32_t linkDownSince_ = 0;
  bool stalled_ = false;  // a transfer was dropped for silence: the next protocol line is answered ERR busy
  uint32_t errorCount_ = 0;
  netprofile::Error lastError_ = netprofile::Error::None;

  void handleLine(uint32_t nowMs);
  void handleCommand(std::string_view command, std::string_view argument, uint32_t nowMs);
  void reply(std::string_view text);
  void replyError(netprofile::Error error);
  void replyFile(std::string_view word, bool found, const std::string& content);
  void dropTransfer();
  void beginTransfer(std::string_view argument, uint32_t nowMs);
  void data(std::string_view payload, uint32_t nowMs);
  void end(uint32_t nowMs);
  void endQuestion(Host::Ended why);
};

// While the Connect to Mac screen is open it owns the serial input; the main loop's own command reader stands aside.
void claimSerialInput(bool claimed);
bool serialInputClaimed();

}  // namespace netprov
