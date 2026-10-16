#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "MethodLibrary.h"
#include "MethodPorts.h"
#include "MethodProtocol.h"
#include "MethodWake.h"

// The check that runs when the Method shelf opens: Wi-Fi, clock, mailbox list, then for each new book
// download and verify. A step machine, not a thread: the shelf calls step() once per main-loop pass,
// every step is short (one Wi-Fi poll, one 2 KB chunk, one SD operation) and the shelf
// handles input between steps. Pure logic over injected ports, covered by
// test/method_check with a fake clock and a fake network.
//
// The mailbox list (GET /books) is read newest first; the reader keeps a high-water mark, the largest book id
// it has dealt with, in the library. Entries above the mark are new and are handled oldest first so the mark
// only ever moves up: a book already on the card (same SHA-256) just advances the mark; a download that
// fails on the network stops the run without advancing past it; a book that fails verification is skipped
// and advanced past, so a bad book is not retried forever; at most three downloads per check, the rest wait
// for the next one. A mailbox whose newest id is below the mark started over: the mark drops to 0 and the
// SHA-256 comparison skips what the card already holds.
//
// Every path out of a running check goes through finish(): the transfer is closed, the
// partial file is removed, the clock sync and the Wi-Fi join are abandoned and the radio is
// switched off, all before finish() returns. cancel() is that path for "the user left".
//
// The same check runs unattended on a timer wake (Limits::wake, see MethodWake.h): then each book also has to pass
// the wake rules (battery, size, time budget, the book on the panel, the retry limit) before it is downloaded, and a
// book that does not stops the run there, like the per-check limit. The shelf leaves the rules off.
namespace method {

enum class Phase : uint8_t { Idle, Wifi, Clock, Mailbox, Download, Verify, Finished };

enum class Result : uint8_t {
  None,           // not finished
  UpToDate,       // asked the mailbox, nothing new
  NewMethod,      // at least one book downloaded, verified and filed
  NotConfigured,  // no mailbox settings
  LowBattery,
  WifiFailed,
  AuthRefused,    // the mailbox answered 401 or 403
  ServerError,    // clock, connection, TLS or an unreadable reply
  Memory,         // not enough heap to start TLS
  LowSpace,
  DownloadFailed,
  VerifyFailed,
  StorageFailed,  // the SD card refused a write
  // The mailbox listed books this reader cannot read and nothing was filed: the reader is out of date.
  UnreadableList,
  Cancelled
};

struct Limits {
  uint32_t wifiLastMs = 4000;      // the network joined last time
  uint32_t wifiScanMs = 10000;     // scan for the other saved networks
  uint32_t wifiAttemptMs = 7000;   // each network found by the scan (as WifiSelectionActivity)
  uint32_t wifiTotalMs = 30000;    // the whole phase
  uint32_t clockMs = 20000;
  uint32_t resolveMs = 8000;       // the name lookup before each request
  uint32_t mailboxMs = 20000;      // list request, start to finish
  // A download may take downloadMs (the request itself: a clock sync and a TLS open share the phase) plus the
  // time the book needs at downloadBytesPerSec. The size term matters: a full 1.9 MB book at the 10.5 KB/s assumed for
  // this link (not measured on a device) takes 180 s, and any fixed limit near that fails a near-maximum book at 99 %
  // every time. The real guard against a dead connection is stallMs, not this.
  uint32_t downloadMs = 60000;
  uint32_t downloadBytesPerSec = 6000;
  uint32_t stallMs = 5000;         // no bytes for this long ends a request
  uint32_t verifyMs = 10000;
  uint64_t spareBytes = 256 * 1024;  // free space required beyond the book itself
  int minBatteryPercent = 15;        // the same floor as the ambient wake
  size_t chunkBytes = 2048;
  uint32_t maxDownloads = 3;       // per check; the rest wait for the next one
  wake::Rules wake;                // inactive unless this is a timer wake's check
};

struct Config {
  bool configured = false;
  bool https = true;  // https needs the clock; the local-development http origin does not
  std::string deviceId;
};

struct Ports {
  ClockPort& clock;
  WifiPort& wifi;
  HttpPort& http;
  FilePort& files;
  HashPort& hash;
  BatteryPort& battery;
  Observer* observer = nullptr;
};

class Check {
  enum class WifiStage : uint8_t { Begin, Last, Scan, Try };
  enum class TransferStage : uint8_t { Sync, Resolve, Open, Read };
  enum class VerifyStage : uint8_t { Digest, Inspect, File };

  Ports ports_;
  Config config_;
  Library& library_;
  Limits limits_;

  Phase phase_ = Phase::Idle;
  Result result_ = Result::None;
  uint32_t revision_ = 0;
  uint32_t lastStepAt_ = 0;
  bool stepped_ = false;
  uint32_t slowestPassMs_ = 0;  // the longest gap between two steps in this phase
  uint32_t phaseStart_ = 0;
  uint32_t lastData_ = 0;
  int64_t finishedAt_ = 0;
  uint64_t neededBytes_ = 0;
  uint32_t percent_ = 0;
  uint32_t reportedStep_ = 0;
  std::string addedPath_;

  WifiStage wifiStage_ = WifiStage::Begin;
  TransferStage transferStage_ = TransferStage::Open;
  VerifyStage verifyStage_ = VerifyStage::Digest;
  uint32_t stageStart_ = 0;
  uint32_t attemptStart_ = 0;
  size_t attempt_ = 0;
  std::vector<size_t> queue_;  // saved Wi-Fi networks still to try
  size_t queued_ = 0;

  std::unique_ptr<uint8_t[]> chunk_;
  std::unique_ptr<char[]> reply_;  // the whole list reply, sized from Content-Length (not std::string: no abort on OOM)
  size_t replyLength_ = 0;
  size_t replyFilled_ = 0;
  std::vector<BookMetadata> list_;  // the mailbox's books, as listed
  std::vector<size_t> todo_;        // indices into list_ above the mark, oldest first
  size_t todoNext_ = 0;
  std::unique_ptr<BookMetadata> book_;  // the one being downloaded
  std::string finalPath_;
  std::string route_;
  uint32_t downloadsStarted_ = 0;
  uint32_t plannedDownloads_ = 0;
  uint32_t startedAt_ = 0;     // nowMs() at start(): the wake budget counts from here
  uint32_t failedBookId_ = 0;  // the book whose download the check ended on, when it ended on one
  std::vector<wake::Decision> decisions_;
  uint32_t filed_ = 0;
  uint32_t unverified_ = 0;
  size_t skippedRows_ = 0;  // entries in the mailbox list that failed validation
  bool moreWaiting_ = false;
  bool markDirty_ = false;
  bool reread_ = false;
  uint32_t received_ = 0;
  char digest_[65] = {};
  bool sinkOpen_ = false;
  bool incoming_ = false;
  bool resolved_ = false;  // the name lookup for the request about to open is done
  bool hashing_ = false;

  void enterPhase(Phase phase);
  void leavePhase(const char* outcome);
  void finish(Result result);
  void teardown();
  bool allocateChunk();
  void touch() { ++revision_; }

  void stepWifi();
  void beginScan();
  void tryNextSaved();
  void joined(size_t saved, bool known);
  void stepClock();
  void stepMailbox();
  void handleReply();
  void nextBook();
  void concludeRun();
  // HTTPS needs a clock synced within the last 90 s; a slow multi-book run can outlast that.
  bool needsFreshClock();
  // True when the request may open now. False means wait (next step) or the check has finished with Memory.
  bool heapAllows();
  void stepSync();
  // Every request starts with a name lookup that can be given up on (see HttpPort::beginResolve).
  void beginResolve();
  void stepResolve();
  void closeTransfer();
  void skipBook();
  void flushMark();
  void beginDownload();
  void stepDownload();
  void stepVerify();
  bool deadlinePassed(uint32_t sinceMs, uint32_t limitMs);
  uint32_t downloadLimitMs() const;
  // A wake check only: may this book be downloaded now? Records the decision either way.
  bool wakeAllows(const BookMetadata& listed, const char* path);
  // Would filing this book delete the wake's protected book (it replaces it, or prunes it)? Asked of a copy of the
  // library, so nothing changes.
  bool displacesProtected(const BookMetadata& listed, const char* path) const;
  // A wake check only: the budget for the whole check has run out.
  bool wakeBudgetSpent();

 public:
  Check(const Ports& ports, Config config, Library& library, Limits limits = {});
  ~Check();
  Check(const Check&) = delete;
  Check& operator=(const Check&) = delete;

  void start();
  void step();
  void cancel();

  bool running() const { return phase_ != Phase::Idle && phase_ != Phase::Finished; }
  Phase phase() const { return phase_; }
  Result result() const { return result_; }
  // Download progress 0 to 100, reported in steps of 10.
  uint32_t percent() const { return percent_; }
  uint64_t neededBytes() const { return neededBytes_; }  // LowSpace: what the card must have free
  int64_t finishedAt() const { return finishedAt_; }     // unix time the check ended, 0 when the clock is unknown
  const std::string& addedPath() const { return addedPath_; }  // the newest book this run filed
  uint32_t filedCount() const { return filed_; }
  // How many entries of the mailbox list this reader could not read (damaged, or from a newer Worker).
  size_t skippedRows() const { return skippedRows_; }
  // Download n of m while a book is coming down (m counts the downloads this check will make).
  uint32_t downloadIndex() const { return downloadsStarted_; }
  uint32_t downloadTotal() const { return plannedDownloads_; }
  Kind currentKind() const { return book_ ? book_->kind : Kind::Method; }
  // New books were left for the next check (the per-check limit, or the run stopped early).
  bool moreWaiting() const { return moreWaiting_; }
  // A wake check: what was decided about each book it came to, in order, downloaded or left for the shelf.
  const std::vector<wake::Decision>& decisions() const { return decisions_; }
  // The id of the book whose download ended the check (DownloadFailed), or 0. A wake counts it against the book.
  uint32_t failedBookId() const { return failedBookId_; }
  // Changes whenever something worth repainting changed.
  uint32_t revision() const { return revision_; }
};

// Did the check reach the mailbox and get an answer? Only those count towards the 60 s gate. UnreadableList is
// not one: it must stay on the status line, not be replaced by "Up to date" when the shelf is opened again.
inline bool reachedMailbox(Result result) { return result == Result::UpToDate || result == Result::NewMethod; }

}  // namespace method
