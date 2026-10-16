#include "MethodCheck.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace method {
namespace {
const char* phaseName(const Phase phase) {
  switch (phase) {
    case Phase::Wifi:
      return "wifi";
    case Phase::Clock:
      return "clock";
    case Phase::Mailbox:
      return "mailbox";
    case Phase::Download:
      return "download";
    case Phase::Verify:
      return "verify";
    case Phase::Idle:
    case Phase::Finished:
      break;
  }
  return "idle";
}

const char* outcomeName(const Result result) {
  switch (result) {
    case Result::None:
      return "continued";
    case Result::UpToDate:
      return "up to date";
    case Result::NewMethod:
      return "new method filed";
    case Result::NotConfigured:
      return "not configured";
    case Result::LowBattery:
      return "low battery";
    case Result::WifiFailed:
      return "wifi failed";
    case Result::AuthRefused:
      return "token refused";
    case Result::ServerError:
      return "server error";
    case Result::Memory:
      return "low memory";
    case Result::LowSpace:
      return "low space";
    case Result::DownloadFailed:
      return "download failed";
    case Result::VerifyFailed:
      return "verify failed";
    case Result::StorageFailed:
      return "sd write failed";
    case Result::UnreadableList:
      return "list not readable";
    case Result::Cancelled:
      return "cancelled";
  }
  return "";
}
}  // namespace

Check::Check(const Ports& ports, Config config, Library& library, Limits limits)
    : ports_(ports), config_(std::move(config)), library_(library), limits_(limits) {}

Check::~Check() { cancel(); }

bool Check::deadlinePassed(const uint32_t sinceMs, const uint32_t limitMs) {
  return static_cast<uint32_t>(ports_.clock.nowMs() - sinceMs) >= limitMs;
}

uint32_t Check::downloadLimitMs() const {
  const uint64_t transferMs =
      static_cast<uint64_t>(book_->byteLength) * 1000 / std::max<uint32_t>(limits_.downloadBytesPerSec, 1);
  return static_cast<uint32_t>(std::min<uint64_t>(limits_.downloadMs + transferMs, UINT32_MAX));
}

bool Check::wakeBudgetSpent() {
  if (!limits_.wake.active || !deadlinePassed(startedAt_, limits_.wake.budgetMs)) return false;
  // Verify is local work with a limit of its own: a book that has arrived is filed, not thrown away.
  switch (phase_) {
    case Phase::Wifi:
      finish(Result::WifiFailed);
      return true;
    case Phase::Clock:
    case Phase::Mailbox:
      finish(Result::ServerError);
      return true;
    case Phase::Download:
      finish(Result::DownloadFailed);
      return true;
    case Phase::Verify:
    case Phase::Idle:
    case Phase::Finished:
      break;
  }
  return false;
}

bool Check::displacesProtected(const BookMetadata& listed, const char* path) const {
  Library trial = library_;
  Entry entry;
  entry.title = listed.title;
  entry.path = path;
  entry.sha256 = listed.sha256;
  entry.bookId = listed.bookId;
  entry.kind = listed.kind;
  entry.receivedAt = ports_.clock.unixNow();
  entry.pageCount = listed.pageCount;
  const std::vector<std::string> garbage = trial.add(std::move(entry));
  return std::find(garbage.begin(), garbage.end(), limits_.wake.protectedPath) != garbage.end();
}

bool Check::wakeAllows(const BookMetadata& listed, const char* path) {
  wake::Request request;
  request.bookId = listed.idNumber;
  request.bytes = listed.byteLength;
  request.battery = ports_.battery.percent();
  request.elapsedMs = static_cast<uint32_t>(ports_.clock.nowMs() - startedAt_);
  request.started = downloadsStarted_;
  request.displacesProtected = !limits_.wake.protectedPath.empty() && displacesProtected(listed, path);
  const wake::Decision decision = wake::judge(limits_.wake, request);
  decisions_.push_back(decision);
  return decision.verdict == wake::Verdict::Download;
}

void Check::start() {
  if (running()) return;
  result_ = Result::None;
  phase_ = Phase::Idle;
  stepped_ = false;
  startedAt_ = ports_.clock.nowMs();
  failedBookId_ = 0;
  decisions_.clear();
  finishedAt_ = 0;
  neededBytes_ = 0;
  percent_ = 0;
  reportedStep_ = 0;
  addedPath_.clear();
  downloadsStarted_ = 0;
  plannedDownloads_ = 0;
  filed_ = 0;
  unverified_ = 0;
  skippedRows_ = 0;
  moreWaiting_ = false;
  markDirty_ = false;
  reread_ = false;
  touch();
  if (!config_.configured) {
    finish(Result::NotConfigured);
    return;
  }
  // The index could not be read, so nothing can be filed: do not download a book that cannot be kept.
  if (library_.readOnly()) {
    finish(Result::StorageFailed);
    return;
  }
  // Before the radio comes on: a flat battery is no place to run Wi-Fi and TLS.
  if (ports_.battery.percent() < limits_.minBatteryPercent) {
    finish(Result::LowBattery);
    return;
  }
  wifiStage_ = WifiStage::Begin;
  enterPhase(Phase::Wifi);
}

void Check::step() {
  if (running()) {
    // The gap since the last step is how long the main loop went without reading input: this step's own time,
    // the rest of the loop pass and the delay. A phase reports its longest.
    const uint32_t now = ports_.clock.nowMs();
    if (stepped_) slowestPassMs_ = std::max(slowestPassMs_, static_cast<uint32_t>(now - lastStepAt_));
    lastStepAt_ = now;
    stepped_ = true;
    if (wakeBudgetSpent()) return;
  }
  switch (phase_) {
    case Phase::Wifi:
      stepWifi();
      break;
    case Phase::Clock:
      stepClock();
      break;
    case Phase::Mailbox:
      stepMailbox();
      break;
    case Phase::Download:
      stepDownload();
      break;
    case Phase::Verify:
      stepVerify();
      break;
    case Phase::Idle:
    case Phase::Finished:
      break;
  }
}

void Check::cancel() {
  if (running()) finish(Result::Cancelled);
}

void Check::enterPhase(const Phase phase) {
  leavePhase("continued");
  phase_ = phase;
  phaseStart_ = ports_.clock.nowMs();
  stageStart_ = phaseStart_;
  slowestPassMs_ = 0;
  touch();
  if (ports_.observer) ports_.observer->phaseEntered(phaseName(phase));

  switch (phase) {
    case Phase::Clock:
      // HTTPS needs the clock for certificate dates. A sync from the last 90 s still counts.
      if (ports_.clock.clockReady()) {
        enterPhase(Phase::Mailbox);
        return;
      }
      ports_.clock.beginSync();
      break;
    case Phase::Mailbox:
      route_ = "/api/device/" + config_.deviceId + "/books";
      reply_.reset();
      replyLength_ = replyFilled_ = 0;
      list_.clear();
      todo_.clear();
      todoNext_ = 0;
      transferStage_ = TransferStage::Open;
      resolved_ = false;
      if (!routeAllowed(config_.deviceId, route_)) finish(Result::ServerError);
      break;
    case Phase::Download:
      beginDownload();
      break;
    case Phase::Verify:
      verifyStage_ = VerifyStage::Digest;
      break;
    case Phase::Wifi:
    case Phase::Idle:
    case Phase::Finished:
      break;
  }
}

void Check::leavePhase(const char* outcome) {
  if (phase_ == Phase::Idle || phase_ == Phase::Finished || !ports_.observer) return;
  ports_.observer->phaseLeft(phaseName(phase_), static_cast<uint32_t>(ports_.clock.nowMs() - phaseStart_), outcome,
                             slowestPassMs_);
}

void Check::teardown() {
  // Close the connection first, while the radio is still up, then the files, then the radio.
  ports_.http.abortResolve();
  ports_.http.close();
  if (sinkOpen_) {
    ports_.files.closeWrite();
    sinkOpen_ = false;
  }
  if (incoming_) {
    ports_.files.remove(INCOMING_PATH);
    incoming_ = false;
  }
  if (hashing_) {
    ports_.hash.abandon();
    hashing_ = false;
  }
  ports_.clock.abortSync();
  ports_.wifi.abortConnect();
  ports_.wifi.off();
  chunk_.reset();
  book_.reset();
  reply_.reset();
  list_ = std::vector<BookMetadata>();
  todo_ = std::vector<size_t>();
  queue_ = std::vector<size_t>();
}

// Mark advances for books the card already holds (or that failed verification) are worth keeping even when
// the run ends early: they are written once, when the run ends. Filing a book writes the mark with it.
void Check::flushMark() {
  if (!markDirty_) return;
  markDirty_ = false;
  library_.save(ports_.files);
}

void Check::finish(const Result result) {
  if (phase_ == Phase::Finished) return;
  if (result == Result::DownloadFailed && book_) failedBookId_ = book_->idNumber;  // teardown() frees the book
  teardown();
  flushMark();
  leavePhase(outcomeName(result));
  result_ = result;
  phase_ = Phase::Finished;
  finishedAt_ = ports_.clock.unixNow();
  touch();
}

bool Check::allocateChunk() {
  if (chunk_) return true;
  chunk_.reset(new (std::nothrow) uint8_t[limits_.chunkBytes]);
  return static_cast<bool>(chunk_);
}

// ---- Wi-Fi ---------------------------------------------------------------------------

void Check::beginScan() {
  ports_.wifi.beginScan();
  wifiStage_ = WifiStage::Scan;
  stageStart_ = ports_.clock.nowMs();
}

void Check::tryNextSaved() {
  if (queued_ >= queue_.size()) {
    finish(Result::WifiFailed);
    return;
  }
  attempt_ = queue_[queued_++];
  ports_.wifi.beginConnect(attempt_);
  attemptStart_ = ports_.clock.nowMs();
  wifiStage_ = WifiStage::Try;
}

void Check::joined(const size_t saved, const bool known) {
  if (known) ports_.wifi.noteJoined(saved);
  enterPhase(config_.https ? Phase::Clock : Phase::Mailbox);
}

void Check::stepWifi() {
  if (deadlinePassed(phaseStart_, limits_.wifiTotalMs)) {
    finish(Result::WifiFailed);
    return;
  }
  switch (wifiStage_) {
    case WifiStage::Begin: {
      if (ports_.wifi.connected()) {
        joined(0, false);
        return;
      }
      const size_t saved = ports_.wifi.savedCount();
      if (saved == 0) {
        finish(Result::WifiFailed);
        return;
      }
      // The network that worked last time first, briefly; then look around.
      const int last = ports_.wifi.lastUsed();
      if (last >= 0 && static_cast<size_t>(last) < saved) {
        attempt_ = static_cast<size_t>(last);
        ports_.wifi.beginConnect(attempt_);
        attemptStart_ = ports_.clock.nowMs();
        wifiStage_ = WifiStage::Last;
      } else {
        beginScan();
      }
      return;
    }
    case WifiStage::Last: {
      const WifiStatus status = ports_.wifi.connectStatus();
      if (status == WifiStatus::Connected) {
        joined(attempt_, true);
      } else if (status == WifiStatus::Failed || deadlinePassed(attemptStart_, limits_.wifiLastMs)) {
        ports_.wifi.abortConnect();
        beginScan();
      }
      return;
    }
    case WifiStage::Scan: {
      const ScanStatus status = ports_.wifi.scanStatus();
      if (status == ScanStatus::Pending) {
        if (deadlinePassed(stageStart_, limits_.wifiScanMs)) finish(Result::WifiFailed);
        return;
      }
      if (status == ScanStatus::Failed) {
        finish(Result::WifiFailed);
        return;
      }
      queue_ = ports_.wifi.visibleSaved();
      queued_ = 0;
      tryNextSaved();
      return;
    }
    case WifiStage::Try: {
      const WifiStatus status = ports_.wifi.connectStatus();
      if (status == WifiStatus::Connected) {
        joined(attempt_, true);
      } else if (status == WifiStatus::Failed || deadlinePassed(attemptStart_, limits_.wifiAttemptMs)) {
        ports_.wifi.abortConnect();
        tryNextSaved();
      }
      return;
    }
  }
}

// ---- Clock ---------------------------------------------------------------------------

void Check::stepClock() {
  const SyncStatus status = ports_.clock.syncStatus();
  if (status == SyncStatus::Synced) {
    enterPhase(Phase::Mailbox);
  } else if (status == SyncStatus::Failed || deadlinePassed(phaseStart_, limits_.clockMs)) {
    // SNTP gave no answer (a network that blocks UDP 123 does this): the retained clock may stand in, as it does for
    // Check connection and the Notes poll. Without it there is no clock for the certificate dates, and no request.
    ports_.clock.abortSync();
    if (ports_.clock.useRetained()) {
      enterPhase(Phase::Mailbox);
      return;
    }
    finish(Result::ServerError);
  }
}

// ---- Mailbox -------------------------------------------------------------------------

// A request over HTTPS refuses to start without a clock synced in the last 90 s (certificate dates). The first
// sync is the Clock phase; a run that takes longer than the window (several slow downloads, or a re-read after a
// 410) syncs again before the next request. True when it started a sync and the caller must wait.
bool Check::needsFreshClock() {
  if (!config_.https || ports_.clock.clockReady()) return false;
  ports_.clock.beginSync();
  transferStage_ = TransferStage::Sync;
  stageStart_ = ports_.clock.nowMs();
  return true;
}

bool Check::heapAllows() {
  if (config_.https) {
    switch (ports_.http.ready()) {
      case HttpReady::Yes:
        break;
      case HttpReady::Wait:
        return false;
      case HttpReady::NoMemory:
        finish(Result::Memory);
        return false;
    }
  }
  if (!allocateChunk()) {
    finish(Result::Memory);
    return false;
  }
  return true;
}

void Check::beginResolve() {
  ports_.http.beginResolve();
  transferStage_ = TransferStage::Resolve;
  stageStart_ = ports_.clock.nowMs();
}

// Back to the Open stage when the name is known. A lookup that fails, or outlasts its limit, is a mailbox that
// cannot be reached.
void Check::stepResolve() {
  switch (ports_.http.resolveStatus()) {
    case ResolveStatus::Done:
      resolved_ = true;
      transferStage_ = TransferStage::Open;
      return;
    case ResolveStatus::Failed:
      finish(Result::ServerError);
      return;
    case ResolveStatus::Pending:
      if (deadlinePassed(stageStart_, limits_.resolveMs)) finish(Result::ServerError);
      return;
  }
}

void Check::stepSync() {
  const SyncStatus status = ports_.clock.syncStatus();
  if (status == SyncStatus::Synced) {
    transferStage_ = TransferStage::Open;
  } else if (status == SyncStatus::Failed || deadlinePassed(stageStart_, limits_.clockMs)) {
    ports_.clock.abortSync();
    if (ports_.clock.useRetained()) {  // as in the Clock phase
      transferStage_ = TransferStage::Open;
      return;
    }
    finish(Result::ServerError);
  }
}

void Check::stepMailbox() {
  if (deadlinePassed(phaseStart_, limits_.mailboxMs)) {
    finish(Result::ServerError);
    return;
  }
  if (transferStage_ == TransferStage::Sync) {
    stepSync();
    return;
  }
  if (transferStage_ == TransferStage::Resolve) {
    stepResolve();
    return;
  }
  if (transferStage_ == TransferStage::Open) {
    if (!resolved_) {
      beginResolve();
      return;
    }
    if (needsFreshClock()) return;
    if (!heapAllows()) return;
    switch (ports_.http.open(route_.c_str(), MAX_LIST_BYTES)) {
      case HttpOpen::Ok:
        break;
      case HttpOpen::Unauthorized:
        finish(Result::AuthRefused);
        return;
      case HttpOpen::NoMemory:
        finish(Result::Memory);
        return;
      case HttpOpen::Gone:
      case HttpOpen::Failed:
        finish(Result::ServerError);
        return;
    }
    const int64_t length = ports_.http.contentLength();
    if (length <= 0 || length > static_cast<int64_t>(MAX_LIST_BYTES)) {
      finish(Result::ServerError);
      return;
    }
    // Sized exactly, once, from the announced length; a failed allocation is a result, not an abort.
    replyLength_ = static_cast<size_t>(length);
    replyFilled_ = 0;
    reply_.reset(new (std::nothrow) char[replyLength_]);
    if (!reply_) {
      finish(Result::Memory);
      return;
    }
    transferStage_ = TransferStage::Read;
    lastData_ = ports_.clock.nowMs();
    return;
  }

  size_t got = 0;
  switch (ports_.http.read(chunk_.get(), limits_.chunkBytes, got)) {
    case HttpRead::Data:
      if (got > replyLength_ - replyFilled_) {
        finish(Result::ServerError);
        return;
      }
      std::memcpy(reply_.get() + replyFilled_, chunk_.get(), got);
      replyFilled_ += got;
      lastData_ = ports_.clock.nowMs();
      return;
    case HttpRead::Idle:
      if (deadlinePassed(lastData_, limits_.stallMs)) finish(Result::ServerError);
      return;
    case HttpRead::Done:
      handleReply();
      return;
    case HttpRead::Failed:
      finish(Result::ServerError);
      return;
  }
}

void Check::handleReply() {
  ports_.http.close();
  uint32_t newest = 0;
  size_t skipped = 0;
  if (replyFilled_ != replyLength_ ||
      !parseBooks(std::string_view(reply_.get(), replyFilled_), config_.deviceId, list_, newest, skipped)) {
    finish(Result::ServerError);
    return;
  }
  reply_.reset();
  skippedRows_ = skipped;
  if (list_.empty()) {
    concludeRun();  // an empty mailbox, or nothing in it this reader can use
    return;
  }
  // The newest id below the mark means the mailbox started over: forget the mark, and let the SHA-256
  // comparison skip whatever the card already holds.
  if (newest < library_.mark()) {
    library_.resetMark();
    markDirty_ = true;
  }
  // Everything above the mark, oldest first, so the mark advances monotonically.
  todo_.clear();
  for (size_t i = 0; i < list_.size(); ++i)
    if (list_[i].idNumber > library_.mark()) todo_.push_back(i);
  std::sort(todo_.begin(), todo_.end(), [&](size_t a, size_t b) { return list_[a].idNumber < list_[b].idNumber; });
  todoNext_ = 0;

  uint32_t needing = 0;
  for (const size_t index : todo_) {
    char path[MAX_PATH_BYTES];
    if (bookPath(list_[index].title, list_[index].sha256, path, sizeof(path)) &&
        !library_.hasVersion(list_[index].sha256, path))
      ++needing;
  }
  plannedDownloads_ = downloadsStarted_ + std::min<uint32_t>(needing, limits_.maxDownloads - downloadsStarted_);
  nextBook();
}

// Move on to the next new book: skip what the card holds, stop at the per-check limit, check space, and
// start the download. Ends the run when nothing is left.
void Check::nextBook() {
  while (todoNext_ < todo_.size()) {
    const BookMetadata& listed = list_[todo_[todoNext_]];
    char path[MAX_PATH_BYTES];
    if (!bookPath(listed.title, listed.sha256, path, sizeof(path))) {
      ++todoNext_;  // cannot be given a file name: not a book this reader can keep
      continue;
    }
    if (library_.hasVersion(listed.sha256, path)) {
      library_.advanceMark(listed.idNumber);  // already here: no download, no change to NEW or progress
      // The timers are part of the send, not of the bytes: a re-send of the same book may bring different ones (or the
      // first ones).
      if (!listed.timersDropped) library_.updateTimers(path, listed.sha256, listed.timers);
      markDirty_ = true;
      ++todoNext_;
      continue;
    }
    // A wake check stops at the first book it will not take, and leaves it and every newer book to the shelf: the
    // mark only moves up, so going on to a newer book would leave this one behind it for good.
    if (limits_.wake.active && !wakeAllows(listed, path)) {
      moreWaiting_ = true;
      break;
    }
    if (downloadsStarted_ >= limits_.maxDownloads) {
      moreWaiting_ = true;
      break;
    }
    const uint64_t needed = static_cast<uint64_t>(listed.byteLength) + limits_.spareBytes;
    const uint64_t available = ports_.files.freeBytes();  // 0 means unknown: go ahead
    if (available != 0 && available < needed) {
      neededBytes_ = needed;
      moreWaiting_ = true;
      finish(Result::LowSpace);
      return;
    }
    book_.reset(new (std::nothrow) BookMetadata(listed));
    if (!book_) {
      finish(Result::Memory);
      return;
    }
    finalPath_ = path;
    ++downloadsStarted_;
    enterPhase(Phase::Download);
    return;
  }
  concludeRun();
}

void Check::concludeRun() {
  // Books that failed verification were skipped; they only decide the outcome when nothing else arrived.
  if (filed_ > 0) {
    finish(Result::NewMethod);
  } else if (unverified_ > 0) {
    finish(Result::VerifyFailed);
  } else if (skippedRows_ > 0) {
    // Rows this reader cannot read were in the list and nothing was filed: "Up to date" would be a lie.
    finish(Result::UnreadableList);
  } else {
    finish(Result::UpToDate);
  }
}

// ---- Download ------------------------------------------------------------------------

void Check::closeTransfer() {
  ports_.http.close();
  if (sinkOpen_) {
    ports_.files.closeWrite();
    sinkOpen_ = false;
  }
  if (incoming_) {
    ports_.files.remove(INCOMING_PATH);
    incoming_ = false;
  }
  if (hashing_) {
    ports_.hash.abandon();
    hashing_ = false;
  }
}

// A book that does not verify is not kept and not retried: the mark moves past it and the run goes on.
void Check::skipBook() {
  ++unverified_;
  library_.advanceMark(book_->idNumber);
  markDirty_ = true;
  closeTransfer();
  ++todoNext_;
  nextBook();
}

void Check::beginDownload() {
  received_ = 0;
  percent_ = 0;
  reportedStep_ = 0;
  transferStage_ = TransferStage::Open;
  resolved_ = false;
  if (!ports_.files.ensureDirectory(DIRECTORY)) {
    finish(Result::StorageFailed);
    return;
  }
  ports_.files.remove(INCOMING_PATH);
  if (!ports_.files.openForWrite(INCOMING_PATH)) {
    finish(Result::StorageFailed);
    return;
  }
  sinkOpen_ = true;
  incoming_ = true;
  if (!ports_.hash.begin()) {
    finish(Result::Memory);
    return;
  }
  hashing_ = true;
}

void Check::stepDownload() {
  if (deadlinePassed(phaseStart_, downloadLimitMs())) {
    finish(Result::DownloadFailed);
    return;
  }
  if (transferStage_ == TransferStage::Sync) {
    stepSync();
    return;
  }
  if (transferStage_ == TransferStage::Resolve) {
    stepResolve();
    return;
  }
  if (transferStage_ == TransferStage::Open) {
    if (!resolved_) {
      beginResolve();
      return;
    }
    if (needsFreshClock()) return;
    if (!heapAllows()) return;
    // The token only ever travels to the configured origin, on this device's own book route.
    if (!routeAllowed(config_.deviceId, book_->bookUrl)) {
      finish(Result::ServerError);
      return;
    }
    switch (ports_.http.open(book_->bookUrl, book_->byteLength)) {
      case HttpOpen::Ok:
        break;
      case HttpOpen::Unauthorized:
        finish(Result::AuthRefused);
        return;
      case HttpOpen::NoMemory:
        finish(Result::Memory);
        return;
      case HttpOpen::Gone:
        // Replaced since the list was read: read the list again, once per check, and carry on from there.
        if (!reread_) {
          reread_ = true;
          --downloadsStarted_;
          closeTransfer();
          book_.reset();
          enterPhase(Phase::Mailbox);
          return;
        }
        finish(Result::DownloadFailed);
        return;
      case HttpOpen::Failed:
        finish(Result::DownloadFailed);
        return;
    }
    // No Content-Length (a chunked reply reads as 0), or one that is not the size the list gave: something between
    // here and the mailbox is wrong. It proves nothing about the book, so the mark does not move past it: the next
    // check asks again.
    if (ports_.http.contentLength() != static_cast<int64_t>(book_->byteLength)) {
      finish(Result::DownloadFailed);
      return;
    }
    transferStage_ = TransferStage::Read;
    lastData_ = ports_.clock.nowMs();
    return;
  }

  size_t got = 0;
  switch (ports_.http.read(chunk_.get(), limits_.chunkBytes, got)) {
    case HttpRead::Data: {
      if (!ports_.files.write(chunk_.get(), got) || !ports_.hash.update(chunk_.get(), got)) {
        finish(Result::StorageFailed);
        return;
      }
      received_ += static_cast<uint32_t>(got);
      if (received_ > book_->byteLength) {
        skipBook();
        return;
      }
      lastData_ = ports_.clock.nowMs();
      const uint32_t step = static_cast<uint32_t>(static_cast<uint64_t>(received_) * 10 / book_->byteLength);
      if (step != reportedStep_) {
        reportedStep_ = step;
        percent_ = step * 10;
        touch();
      }
      return;
    }
    case HttpRead::Idle:
      if (deadlinePassed(lastData_, limits_.stallMs)) finish(Result::DownloadFailed);
      return;
    case HttpRead::Done:
      ports_.http.close();
      sinkOpen_ = false;
      if (!ports_.files.closeWrite()) {
        finish(Result::StorageFailed);
        return;
      }
      enterPhase(Phase::Verify);
      return;
    case HttpRead::Failed:
      finish(Result::DownloadFailed);
      return;
  }
}

// ---- Verify and file -----------------------------------------------------------------

void Check::stepVerify() {
  if (deadlinePassed(phaseStart_, limits_.verifyMs)) {
    skipBook();
    return;
  }
  switch (verifyStage_) {
    case VerifyStage::Digest:
      hashing_ = false;  // finish() releases the context either way
      if (!ports_.hash.finish(digest_) || received_ != book_->byteLength || std::strcmp(digest_, book_->sha256) != 0) {
        skipBook();
        return;
      }
      verifyStage_ = VerifyStage::Inspect;
      return;
    case VerifyStage::Inspect: {
      BookInfo info;
      if (!ports_.files.inspectBook(INCOMING_PATH, info) || info.bytes != book_->byteLength || !info.magicOk ||
          !info.parsed || info.pageCount == 0) {
        skipBook();
        return;
      }
      // Hand the parsed page count to the File stage through the metadata (the parser is the truth).
      book_->pageCount = static_cast<uint16_t>(info.pageCount > UINT16_MAX ? UINT16_MAX : info.pageCount);
      verifyStage_ = VerifyStage::File;
      return;
    }
    case VerifyStage::File: {
      // A new version has a new name, so nothing keyed by the old path can go stale; a leftover
      // cache for this exact name is cleared so reading starts at page 1.
      ports_.files.removeBookCache(finalPath_.c_str());
      if (!ports_.files.replaceFile(INCOMING_PATH, finalPath_.c_str())) {
        finish(Result::StorageFailed);
        return;
      }
      incoming_ = false;

      const Library before = library_;
      library_.advanceMark(book_->idNumber);  // written with the entry, in the same index
      Entry entry;
      entry.title = book_->title;
      entry.path = finalPath_;
      entry.sha256 = book_->sha256;
      entry.bookId = book_->bookId;
      entry.kind = book_->kind;
      entry.receivedAt = ports_.clock.unixNow();
      entry.pageCount = book_->pageCount;
      entry.timers = book_->timers;  // already validated by the list parser (all of a book's timers, or none)
      std::vector<std::string> garbage = library_.add(std::move(entry));
      // The book a wake leaves on the panel is not deleted by a wake. wakeAllows() already refused a book that would
      // delete it; this is the last guard, should the library have changed since. The file stays on the card but is no
      // longer in the index, and Library::load does not necessarily bring it back: it adopts an unindexed file only
      // into a free slot, and never under a title that is already on the shelf. So after a same-title replacement the
      // file is an orphan (the button wake still reopens it, by its path) until that title leaves the shelf, and after
      // a prune it is adopted once there is room. Nothing deletes it.
      if (limits_.wake.active && !limits_.wake.protectedPath.empty())
        garbage.erase(std::remove(garbage.begin(), garbage.end(), limits_.wake.protectedPath), garbage.end());
      // The old version and any pruned books are deleted only once the new index is on the card.
      if (!library_.commit(ports_.files, garbage)) {
        library_ = before;
        ports_.files.removeBook(finalPath_.c_str());
        finish(Result::StorageFailed);
        return;
      }
      markDirty_ = false;
      ++filed_;
      addedPath_ = finalPath_;
      ++todoNext_;
      touch();
      nextBook();
      return;
    }
  }
}

}  // namespace method
