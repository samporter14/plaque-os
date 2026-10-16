#include "MethodActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "ambient/AmbientSettings.h"
#include "ambient/MethodLayout.h"
#include "ambient/MethodProtocol.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

namespace fui = freeink::ui;

namespace {
constexpr unsigned long LONG_PRESS_MS = 1000;
// A check that reached the mailbox this recently is not repeated when the shelf is opened again.
constexpr uint32_t RECENT_CHECK_MS = 60000;
// The first paint comes before any network work; this only bounds the wait if a render never reports back.
constexpr uint32_t FIRST_PAINT_WAIT_MS = 3000;
// A finger position older than this is not the finger that opened the dialog.
constexpr uint32_t FINGER_FRESH_MS = 1000;
constexpr int SIDE_PADDING = 12;
constexpr int STATUS_PADDING = 6;
constexpr int STATUS_MAX_LINES = 2;

// The last check that reached the mailbox with nothing left waiting. Lives as long as the firmware runs,
// because the shelf is destroyed every time a book opens.
struct LastCheck {
  bool valid = false;
  uint32_t atMs = 0;
  int64_t finishedAt = 0;
};
LastCheck lastCheck;

std::string lowerFirst(const char* text) {
  std::string out = text;
  if (!out.empty() && out[0] >= 'A' && out[0] <= 'Z') out[0] = static_cast<char>(out[0] - 'A' + 'a');
  return out;
}
}  // namespace

MethodActivity::MethodActivity(GfxRenderer& renderer, MappedInputManager& input, std::string selectPath)
    : UiListActivity("Method", renderer, input, true), selectPath(std::move(selectPath)) {}

// ---- lifecycle -----------------------------------------------------------------------

void MethodActivity::fillWords() {
  words.oneStep = tr(STR_METHOD_STEP_ONE);
  words.steps = tr(STR_METHOD_STEPS_FORMAT);
  words.received = tr(STR_METHOD_RECEIVED_FORMAT);
  words.today = tr(STR_METHOD_TODAY);
  words.notStarted = tr(STR_METHOD_NOT_STARTED);
  words.stepOf = tr(STR_METHOD_STEP_OF_FORMAT);
  words.finished = tr(STR_METHOD_FINISHED);
  words.onePage = tr(STR_METHOD_ONE_PAGE);
  words.pages = tr(STR_METHOD_PAGES_FORMAT);
  const char* kinds[4] = {tr(STR_METHOD), tr(STR_METHOD_KIND_TABLE), tr(STR_METHOD_KIND_PLATE),
                          tr(STR_METHOD_KIND_BOARD)};
  std::copy(kinds, kinds + 4, words.kinds);
  for (int i = 0; i < 4; ++i) kindNamesLower[i] = lowerFirst(kinds[i]);
  const char* weekdays[7] = {tr(STR_METHOD_SUN), tr(STR_METHOD_MON), tr(STR_METHOD_TUE), tr(STR_METHOD_WED),
                             tr(STR_METHOD_THU), tr(STR_METHOD_FRI), tr(STR_METHOD_SAT)};
  const char* months[12] = {tr(STR_METHOD_JAN), tr(STR_METHOD_FEB), tr(STR_METHOD_MAR), tr(STR_METHOD_APR),
                            tr(STR_METHOD_MAY), tr(STR_METHOD_JUN), tr(STR_METHOD_JUL), tr(STR_METHOD_AUG),
                            tr(STR_METHOD_SEP), tr(STR_METHOD_OCT), tr(STR_METHOD_NOV), tr(STR_METHOD_DEC)};
  std::copy(weekdays, weekdays + 7, words.weekdays);
  std::copy(months, months + 12, words.months);
}

void MethodActivity::onEnter() {
  // One lock across the whole entry (as LibraryListActivity does): a stale render request from the previous
  // screen must not reach render() before the UI host is reset and the rows exist.
  RenderLock lock(*this);
  enteredAt = millis();
  ports = makeUniqueNoThrow<method::DevicePorts>(renderer);
  if (!ports) {
    LOG_ERR("METHOD", "OOM: shelf");
    UiListActivity::onEnter();
    leaving = true;
    onGoHome(HomeMenuItem::METHOD);
    return;
  }
  fillWords();

  // Local state first: the shelf is painted from the card, with no network.
  const auto report = library.load(ports->files);
  LOG_INF("METHOD",
          "shelf: %u books, mark %u (index read=%d recovered=%d dropped=%u adopted=%u opened=%u migrated=%d saved=%d)",
          static_cast<unsigned>(library.entries().size()), static_cast<unsigned>(library.mark()), report.indexRead,
          report.recovered, static_cast<unsigned>(report.dropped), static_cast<unsigned>(report.adopted),
          static_cast<unsigned>(report.opened), report.migrated, report.saved);
  if (report.indexUnreadable)
    LOG_ERR("METHOD", "index.json could not be read: the shelf shows the folder only and writes nothing this visit");
  if (report.indexDamaged)
    LOG_ERR("METHOD", "index.json is not an index: kept as index.json.bad%s",
            report.saved ? ", rebuilt from the folder" : ", and left in place (no rewrite this visit)");
  rebuildRows();
  decideCheck();

  UiListActivity::onEnter();
  app.on(ACTION_STATUS, &MethodActivity::statusTrampoline, this);
  const int selected = indexOfPath(selectPath);
  if (selected > 0) activeNav().requestSelection(selected);
  lastSelected = activeNav().selected.load();
  // Launched from Home while Confirm is still down: its release must not open the first row.
  lockNextConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
}

void MethodActivity::onExit() {
  leaving = true;
  removePopup.dismiss();
  // Transfer aborted, partial file removed, Wi-Fi off, buffers freed: all before the next screen starts.
  cancelCheck();
  UiListActivity::onExit();
}

bool MethodActivity::handleHomeGesture() {
  // ActivityManager checks the Home gesture before loop(); stop the check now so it cannot step once more.
  leaving = true;
  cancelCheck();
  return false;  // the usual go-home follows
}

void MethodActivity::cancelCheck() {
  if (!check) return;
  check->cancel();
  check.reset();
}

// ---- the check -----------------------------------------------------------------------

void MethodActivity::decideCheck() {
  Status status;
  if (!AMBIENT_SETTINGS.configured()) {
    status.kind = StatusKind::NotSetUp;
  } else if (library.readOnly()) {
    // The index on the card could not be read, so a book could not be filed: say so, and download nothing.
    status.kind = StatusKind::SdCard;
  } else if (lastCheck.valid && static_cast<uint32_t>(millis() - lastCheck.atMs) < RECENT_CHECK_MS) {
    status.kind = StatusKind::UpToDate;
    snprintf(status.time, sizeof(status.time), "%s", timeText(lastCheck.finishedAt).c_str());
  } else {
    status.kind = StatusKind::Joining;
    wantsCheck = true;
    WIFI_STORE.loadFromFile();  // the saved networks, read once before the first paint
  }
  publishStatus(status);
}

// Tap on the status line, or Confirm on it: check again now, unless one is already running.
void MethodActivity::recheck() {
  if (leaving || wantsCheck || (check && check->running()) || !AMBIENT_SETTINGS.configured() || library.readOnly())
    return;
  {
    RenderLock lock(*this);  // as WifiSelectionActivity does around its read of the saved networks
    WIFI_STORE.loadFromFile();
  }
  wantsCheck = true;
  Status status;
  status.kind = StatusKind::Joining;
  publishStatus(status);
}

void MethodActivity::statusTrampoline(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<MethodActivity*>(user);
  // A tap on the status line is a touch: if the layout under the finger is not the one on the panel yet (the line
  // grows to 64 px when a check ends), it was aimed at the old frame, and the old frame had no status line there.
  if (!self->frameOnPanel.load()) return;
  self->recheck();
}

void MethodActivity::maybeStartCheck() {
  if (!wantsCheck || check) return;
  if (!rendered.load() && static_cast<uint32_t>(millis() - enteredAt) < FIRST_PAINT_WAIT_MS) return;
  startCheck();
}

void MethodActivity::startCheck() {
  wantsCheck = false;
  const auto& settings = AMBIENT_SETTINGS;
  method::Config config;
  config.configured = settings.configured();
  config.https = std::strncmp(settings.baseUrl, "https://", 8) == 0;
  config.deviceId = settings.deviceId;
  const method::Ports wiring{ports->clock, ports->wifi, ports->http, ports->files, ports->hash, ports->battery,
                             &ports->log};
  check = makeUniqueNoThrow<method::Check>(wiring, std::move(config), library);
  if (!check) {
    LOG_ERR("METHOD", "OOM: check");
    Status status;
    status.kind = StatusKind::Memory;
    publishStatus(status);
    return;
  }
  check->start();
  if (check->running()) {
    publishStatus(statusFromCheck());
    lastRevision = check->revision();
  } else {
    finishCheck();
  }
}

void MethodActivity::stepCheck() {
  if (!check || !check->running()) return;
  const uint32_t filedBefore = check->filedCount();
  check->step();
  if (!check->running()) {
    finishCheck();
    return;
  }
  // Books are filed one at a time; show each as soon as it is on the card.
  if (check->filedCount() != filedBefore) showFiledBooks();
  if (check->revision() != lastRevision) {
    lastRevision = check->revision();
    publishStatus(statusFromCheck());
  }
}

void MethodActivity::finishCheck() {
  const method::Result result = check->result();
  LOG_INF("METHOD", "check finished: result=%d filed=%u mark=%u more_waiting=%d unreadable_rows=%u",
          static_cast<int>(result), static_cast<unsigned>(check->filedCount()), static_cast<unsigned>(library.mark()),
          check->moreWaiting(), static_cast<unsigned>(check->skippedRows()));
  // Only a check that reached the mailbox and left nothing waiting excuses the next visit from checking.
  if (method::reachedMailbox(result) && !check->moreWaiting()) lastCheck = {true, millis(), check->finishedAt()};
  if (library.overLimit() > 0)
    LOG_INF("METHOD", "the shelf holds %u books, %u over the limit of %u: only hand-copied books are left to prune",
            static_cast<unsigned>(library.entries().size()), static_cast<unsigned>(library.overLimit()),
            static_cast<unsigned>(method::MAX_METHODS));
  const Status status = statusFromCheck();
  if (check->filedCount() > 0) showFiledBooks();
  publishStatus(status);
  check.reset();
}

// New books are on the card: put them on the shelf. The newest goes on top. The selection follows it only if the
// user has not moved the selection; otherwise it stays on the book it was on, wherever that book now sits.
void MethodActivity::showFiledBooks() {
  const std::string before = selectedPath();
  rebuildRows();
  int target = userMovedSelection ? indexOfPath(before) : indexOfPath(check->addedPath());
  if (target < 0) target = 0;
  activeNav().requestSelection(target);
  lastSelected = target;
}

MethodActivity::Status MethodActivity::statusFromCheck() const {
  Status status;
  if (check->running()) {
    switch (check->phase()) {
      case method::Phase::Wifi:
        status.kind = StatusKind::Joining;
        break;
      case method::Phase::Clock:
        status.kind = StatusKind::SettingClock;
        break;
      case method::Phase::Mailbox:
        status.kind = StatusKind::Checking;
        break;
      case method::Phase::Download:
        status.kind = StatusKind::Downloading;
        status.percent = static_cast<uint8_t>(check->percent());
        status.count = static_cast<uint8_t>(check->downloadIndex());
        status.total = static_cast<uint8_t>(check->downloadTotal());
        status.bookKind = check->currentKind();
        break;
      case method::Phase::Verify:
        status.kind = StatusKind::Verifying;
        break;
      case method::Phase::Idle:
      case method::Phase::Finished:
        break;
    }
    return status;
  }
  switch (check->result()) {
    case method::Result::UpToDate:
      status.kind = StatusKind::UpToDate;
      break;
    case method::Result::NewMethod: {
      status.kind = StatusKind::NewMethod;
      status.count = static_cast<uint8_t>(std::min<uint32_t>(check->filedCount(), 255));
      const auto* added = library.find(check->addedPath());
      if (added) status.bookKind = added->kind;
      break;
    }
    case method::Result::NotConfigured:
      status.kind = StatusKind::NotSetUp;
      break;
    case method::Result::LowBattery:
      status.kind = StatusKind::LowBattery;
      break;
    case method::Result::WifiFailed:
      status.kind = StatusKind::NoWifi;
      break;
    case method::Result::AuthRefused:
      status.kind = StatusKind::Auth;
      break;
    case method::Result::ServerError:
      status.kind = StatusKind::Server;
      break;
    case method::Result::Memory:
      status.kind = StatusKind::Memory;
      break;
    case method::Result::LowSpace:
      status.kind = StatusKind::LowSpace;
      snprintf(status.needed, sizeof(status.needed), "%s", method::megabytesText(check->neededBytes()).c_str());
      break;
    case method::Result::DownloadFailed:
      status.kind = StatusKind::DownloadFailed;
      break;
    case method::Result::VerifyFailed:
      status.kind = StatusKind::VerifyFailed;
      break;
    case method::Result::StorageFailed:
      status.kind = StatusKind::SdCard;
      break;
    case method::Result::UnreadableList:
      status.kind = StatusKind::Unreadable;
      break;
    case method::Result::None:
    case method::Result::Cancelled:
      break;
  }
  snprintf(status.time, sizeof(status.time), "%s", timeText(check->finishedAt()).c_str());
  return status;
}

std::string MethodActivity::timeText(const int64_t epoch) const {
  method::LocalTime local;
  if (!ports || !ports->clock.localTime(epoch, local)) return {};
  return method::clockText(local, SETTINGS.clockFormat == 1);
}

void MethodActivity::publishStatus(const Status& status) {
  {
    std::lock_guard<std::mutex> lock(viewMutex);
    publishedStatus = status;
  }
  requestUpdate();
}

// ---- rows ----------------------------------------------------------------------------

void MethodActivity::rebuildRows() {
  auto fresh = std::make_shared<Rows>();
  method::LocalTime now;
  ports->clock.localTime(ports->clock.unixNow(), now);
  const bool twelveHour = SETTINGS.clockFormat == 1;
  for (const auto& entry : library.entries()) {
    Row row;
    row.title = entry.title;
    row.path = entry.path;
    row.isNew = !entry.opened;
    method::LocalTime received;
    ports->clock.localTime(entry.receivedAt, received);
    row.detail = method::rowDetail(entry.kind, entry.pageCount, received, now, twelveHour,
                                   ports->files.savedPage(entry.path.c_str()), words);
    fresh->push_back(std::move(row));
  }
  rows = fresh;
  {
    std::lock_guard<std::mutex> lock(viewMutex);
    publishedRows = fresh;
  }
  rowCount.store(static_cast<int>(fresh->size()));
  requestUpdate();
}

int MethodActivity::indexOfPath(const std::string& path) const {
  if (!rows || path.empty()) return -1;
  for (size_t i = 0; i < rows->size(); ++i)
    if ((*rows)[i].path == path) return static_cast<int>(i);
  return -1;
}

std::string MethodActivity::selectedPath() const {
  const int selected = const_cast<MethodActivity*>(this)->activeNav().selected.load();
  if (!rows || selected < 0 || selected >= static_cast<int>(rows->size())) return {};
  return (*rows)[static_cast<size_t>(selected)].path;
}

std::shared_ptr<const MethodActivity::Rows> MethodActivity::renderedRows() const {
  std::lock_guard<std::mutex> lock(viewMutex);
  return drawnRows;
}

void MethodActivity::trackSelection() {
  const int selected = activeNav().selected.load();
  if (selected != lastSelected) {
    lastSelected = selected;
    userMovedSelection = true;
  }
}

void MethodActivity::focusStatus(const bool focused) {
  if (statusFocused.load() == focused) return;
  statusFocused.store(focused);
  if (focused) userMovedSelection = true;
  requestUpdate();
}

// The live finger position, sampled every pass: a long-press fires while the finger is still down, so the
// last sample is where it is. The remove dialog uses it to put Keep under the finger.
void MethodActivity::sampleFinger() {
  int x = 0;
  int y = 0;
  if (mappedInput.hasTouch() && mappedInput.isScreenTouchHeld(x, y)) finger = {x, y, millis(), true};
}

// ---- input ---------------------------------------------------------------------------

void MethodActivity::loop() {
  if (leaving) return;
  sampleFinger();
  UiListActivity::loop();
  if (leaving) return;
  trackSelection();
  maybeStartCheck();
  stepCheck();
  if (ports) ports->wifi.writePendingNote();
}

bool MethodActivity::handleCustomInput() {
  if (removePopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (lockNextConfirmRelease && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    lockNextConfirmRelease = false;
    return true;
  }
  return false;
}

bool MethodActivity::handleButtons() {
  // The hold fires at the threshold and consumes its own release (ActivityManager::loop), so the
  // release cannot land in the dialog it opened.
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, LONG_PRESS_MS)) {
    const int selected = activeNav().selected.load();
    if (!statusFocused.load() && rows && selected >= 0 && selected < static_cast<int>(rows->size()))
      promptRemove((*rows)[static_cast<size_t>(selected)].path, (*rows)[static_cast<size_t>(selected)].title, false);
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onBackButton();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (statusFocused.load()) {
      recheck();
    } else {
      activateIndex(activeNav().selected.load());
    }
    return true;
  }
  return false;
}

// The keys walk a ring: the status line, then the rows. Up from the first row lands on the status line (where
// Confirm checks again) and Down from it goes back to the first row; a hold pages through the rows.
void MethodActivity::navigateButtons() {
  const int count = listCount();
  const int ring = count + 1;
  const int position = statusFocused.load() ? 0 : activeNav().selected.load() + 1;
  const auto goTo = [this](const int target) {
    if (target == 0) {
      focusStatus(true);
    } else {
      focusStatus(false);
      moveSelectionTo(method::rowForRing(target));
    }
  };
  buttonNavigator.onNextPress([&] { goTo(ButtonNavigator::nextIndex(position, ring)); });
  buttonNavigator.onPreviousPress([&] { goTo(ButtonNavigator::previousIndex(position, ring)); });
  auto& nav = activeNav();
  buttonNavigator.onNextContinuous([&] {
    if (count == 0) return;
    focusStatus(false);
    moveSelectionTo(ButtonNavigator::nextPageIndex(nav.selected.load(), count, nav.inputPageRows()));
  });
  buttonNavigator.onPreviousContinuous([&] {
    if (count == 0) return;
    focusStatus(false);
    moveSelectionTo(ButtonNavigator::previousPageIndex(nav.selected.load(), count, nav.inputPageRows()));
  });
}

void MethodActivity::onBackButton() {
  leaving = true;
  cancelCheck();
  app.clearTapFlash();
  // Explicit item: Home must come back with Method selected even though this activity is leaving.
  onGoHome(HomeMenuItem::METHOD);
}

void MethodActivity::activateIndex(const int index) {
  if (!rows || index < 0 || index >= static_cast<int>(rows->size())) return;
  openPath((*rows)[static_cast<size_t>(index)].path);
}

void MethodActivity::onRowLongPress(const int index) {
  if (!rows || index < 0 || index >= static_cast<int>(rows->size())) return;
  promptRemove((*rows)[static_cast<size_t>(index)].path, (*rows)[static_cast<size_t>(index)].title, true);
}

// A tap names a row of the frame that was on screen. If a new book slid in since, the same index is a
// different book now, so the row is resolved against what was drawn, never against the current list.
void MethodActivity::onRowAction(const fui::ActionEvent& event) {
  if (!frameOnPanel.load()) return;  // a frame with another layout is still being drawn: the tap is not for it
  const auto shown = renderedRows();
  if (!shown || !rows) return;
  std::vector<std::string> drawnPaths;
  std::vector<std::string> currentPaths;
  for (const Row& row : *shown) drawnPaths.push_back(row.path);
  for (const Row& row : *rows) currentPaths.push_back(row.path);
  const int current = method::resolveDrawnRow(drawnPaths, event.value, currentPaths);
  if (current < 0) return;  // out of range, or removed in the meantime
  const Row row = (*rows)[static_cast<size_t>(current)];
  focusStatus(false);
  activeNav().requestSelection(current);
  lastSelected = current;
  if (event.longPress) {
    promptRemove(row.path, row.title, true);
  } else {
    openPath(row.path);
  }
}

void MethodActivity::openPath(const std::string& path) {
  if (leaving) return;
  leaving = true;
  app.clearTapFlash();
  // The reader replaces this screen; whatever the check holds (connection, partial file, Wi-Fi, heap) goes first.
  cancelCheck();
  if (library.markOpened(path) && !library.save(ports->files))
    LOG_ERR("METHOD", "Could not record that %s was opened", path.c_str());
  LOG_INF("METHOD", "Opening %s (free=%u max_block=%u)", path.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
  // The XTC reader restores the saved page of this file through the normal path (Recents and progress included).
  onSelectBook(path);
}

void MethodActivity::promptRemove(const std::string& path, const std::string& title, const bool byTouch) {
  app.clearTapFlash();
  const bool hasFinger = byTouch && finger.valid && static_cast<uint32_t>(millis() - finger.at) < FINGER_FRESH_MS;
  // Inline (not a pushed activity) so the check keeps stepping while the dialog is up. The callback holds
  // the path, not a row index: a new book may land while the dialog is open.
  removePopup.show(tr(STR_METHOD_REMOVE_TITLE), title.c_str(), tr(STR_METHOD_KEEP), tr(STR_METHOD_REMOVE), hasFinger,
                   finger.x, finger.y, [this, path] { removeMethod(path); });
  requestUpdate();
}

void MethodActivity::removeMethod(const std::string& path) {
  const int before = activeNav().selected.load();
  // File, reading cache and Recent Books entry go once the index no longer lists the book.
  switch (library.removeAndCommit(ports->files, path)) {
    case method::Library::RemoveResult::NotListed:
      return;
    case method::Library::RemoveResult::WriteFailed: {
      // The library was put back as it was: the book is still on the shelf, as it is on the card.
      LOG_ERR("METHOD", "Could not write the index after removing %s: the book stays", path.c_str());
      Status status;
      status.kind = StatusKind::SdCard;
      publishStatus(status);
      return;
    }
    case method::Library::RemoveResult::Removed:
      break;
  }
  rebuildRows();
  const int count = static_cast<int>(rows->size());
  const int target = count == 0 ? 0 : std::min(before, count - 1);
  activeNav().requestSelection(target);
  lastSelected = target;
}

// ---- drawing -------------------------------------------------------------------------

const char* MethodActivity::headerTitle() const { return tr(STR_METHOD); }

std::string MethodActivity::statusText(const Status& status) const {
  char text[192];
  const auto withTime = [&](StrId withTimeId, StrId plainId) -> std::string {
    if (!status.time[0]) return I18N.get(plainId);
    snprintf(text, sizeof(text), I18N.get(withTimeId), status.time);
    return text;
  };
  const std::string& kind = kindNamesLower[static_cast<size_t>(status.bookKind)];
  switch (status.kind) {
    case StatusKind::None:
      return {};
    case StatusKind::Joining:
      return tr(STR_METHOD_STATUS_WIFI);
    case StatusKind::SettingClock:
      return tr(STR_METHOD_STATUS_CLOCK);
    case StatusKind::Checking:
      return tr(STR_METHOD_STATUS_MAILBOX);
    case StatusKind::Downloading:
      if (status.total > 1) {
        snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_DOWNLOAD_OF), static_cast<int>(status.count),
                 static_cast<int>(status.total), static_cast<int>(status.percent));
      } else {
        snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_DOWNLOAD), kind.c_str(), static_cast<int>(status.percent));
      }
      return text;
    case StatusKind::Verifying:
      return tr(STR_METHOD_STATUS_VERIFY);
    case StatusKind::UpToDate:
      return withTime(StrId::STR_METHOD_STATUS_UP_TO_DATE, StrId::STR_METHOD_STATUS_UP_TO_DATE_NO_TIME);
    case StatusKind::NewMethod:
      if (status.count > 1) {
        if (!status.time[0]) {
          snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_NEW_MANY_NO_TIME), static_cast<int>(status.count));
        } else {
          snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_NEW_MANY), static_cast<int>(status.count), status.time);
        }
        return text;
      }
      if (status.bookKind != method::Kind::Method) {
        if (!status.time[0]) {
          snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_NEW_KIND_NO_TIME), kind.c_str());
        } else {
          snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_NEW_KIND), kind.c_str(), status.time);
        }
        return text;
      }
      return withTime(StrId::STR_METHOD_STATUS_NEW, StrId::STR_METHOD_STATUS_NEW_NO_TIME);
    case StatusKind::NoWifi:
      return tr(STR_METHOD_STATUS_NO_WIFI);
    case StatusKind::Auth:
      return tr(STR_METHOD_STATUS_AUTH);
    case StatusKind::Server:
      return tr(STR_METHOD_STATUS_SERVER);
    case StatusKind::Memory:
      return tr(STR_METHOD_ERR_MEMORY);
    case StatusKind::SdCard:
      return tr(STR_METHOD_ERR_STORAGE);
    case StatusKind::DownloadFailed:
      snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_DOWNLOAD_FAILED), tr(STR_METHOD));
      return text;
    case StatusKind::VerifyFailed:
      return tr(STR_METHOD_STATUS_VERIFY_FAILED);
    case StatusKind::LowSpace:
      snprintf(text, sizeof(text), tr(STR_METHOD_STATUS_LOW_SPACE), status.needed);
      return text;
    case StatusKind::LowBattery:
      return tr(STR_METHOD_STATUS_LOW_BATTERY);
    case StatusKind::NotSetUp:
      return tr(STR_METHOD_STATUS_NOT_SET_UP);
    case StatusKind::Unreadable:
      return tr(STR_METHOD_STATUS_UNREADABLE);
  }
  return {};
}

// Render task. With nothing on the shelf, a running check says what it is doing in the body instead, so the
// same words are not shown twice. The band is a touch target (tap = check again) and so at least 64 px tall
// whenever no check is running.
void MethodActivity::layoutStatusBand() {
  bandLines.clear();
  bandHeight = 0;
  if (!viewRows || (viewRows->empty() && isBusy(viewStatus.kind))) return;
  const std::string text = statusText(viewStatus);
  if (text.empty()) return;
  const int width =
      renderer.getScreenWidth() - 2 * (UITheme::getInstance().getMetrics().contentSidePadding + SIDE_PADDING);
  bandLines = renderer.wrappedText(SMALL_FONT_ID, text.c_str(), width, STATUS_MAX_LINES);
  const int natural = static_cast<int>(bandLines.size()) * renderer.getLineHeight(SMALL_FONT_ID) + 2 * STATUS_PADDING;
  const int target = mappedInput.hasTouch() && !isBusy(viewStatus.kind) ? method::MIN_ROW_HEIGHT_PX : 0;
  bandHeight = std::max(natural, target);
}

void MethodActivity::render(RenderLock&& lock) {
  if (removePopup.processRender(renderer, mappedInput)) return;
  {
    std::lock_guard<std::mutex> guard(viewMutex);
    viewRows = publishedRows;
    viewStatus = publishedStatus;
  }
  layoutStatusBand();
  UiListActivity::render(std::move(lock));
  // displayBuffer() has returned: this frame is on the panel, so taps now resolve against its rows.
  {
    std::lock_guard<std::mutex> guard(viewMutex);
    drawnRows = viewRows;
  }
  displayedLayout = builtLayout;
  frameOnPanel.store(true);
  rendered.store(true);
}

void MethodActivity::drawChrome() {
  UiListActivity::drawChrome();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int margin = metrics.contentSidePadding + SIDE_PADDING;
  const int top = metrics.topPadding + metrics.headerHeight;

  if (bandHeight > 0) {
    const bool focused = statusFocused.load();
    if (focused) renderer.fillRect(0, top, renderer.getScreenWidth(), bandHeight, true);
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    int y = top + (bandHeight - static_cast<int>(bandLines.size()) * lineHeight) / 2;
    for (const auto& line : bandLines) {
      renderer.drawText(SMALL_FONT_ID, margin, y, line.c_str(), !focused);
      y += lineHeight;
    }
  }

  if (viewRows && viewRows->empty()) {
    // Empty shelf: what is missing, and how to get one (or, while the check runs, what it is doing).
    const int width = renderer.getScreenWidth() - 2 * margin;
    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const std::string second =
        isBusy(viewStatus.kind) ? statusText(viewStatus) : std::string(tr(STR_METHOD_EMPTY_HINT));
    const auto first = renderer.wrappedText(UI_10_FONT_ID, tr(STR_METHOD_EMPTY), width, 2);
    const auto rest = renderer.wrappedText(UI_10_FONT_ID, second.c_str(), width, 3);
    const int block = static_cast<int>(first.size() + rest.size()) * lineHeight + metrics.verticalSpacing;
    const int bodyTop = top + bandHeight;
    const int bodyHeight = renderer.getScreenHeight() - bodyTop - metrics.buttonHintsHeight;
    int textY = bodyTop + (bodyHeight > block ? (bodyHeight - block) / 2 : 0);
    for (const auto& line : first) {
      renderer.drawCenteredText(UI_10_FONT_ID, textY, line.c_str(), true, EpdFontFamily::BOLD);
      textY += lineHeight;
    }
    textY += metrics.verticalSpacing;
    for (const auto& line : rest) {
      renderer.drawCenteredText(UI_10_FONT_ID, textY, line.c_str());
      textY += lineHeight;
    }
  }
}

void MethodActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const bool hasRows = viewRows && !viewRows->empty();
  // The hold hint sits above the key hints (the X4 Pro has both touch and keys, and gloves defeat touch).
  const int holdHint = hasRows ? renderer.getLineHeight(SMALL_FONT_ID) + metrics.verticalSpacing : 0;
  const int top = metrics.topPadding + metrics.headerHeight;
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(top + bandHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight + holdHint), 0});
  if (bandHeight > 0) {
    // The status line is a row of its own for touch: tap it to check again.
    screen.frame().hit(fui::Rect{0, static_cast<int16_t>(top), static_cast<int16_t>(renderer.getScreenWidth()),
                                 static_cast<int16_t>(bandHeight)},
                       ACTION_STATUS, 0, fui::InputTouch);
  }
  if (!hasRows) {
    noteBuiltLayout(0);
    return;
  }

  items.clear();
  items.reserve(viewRows->size());
  for (size_t i = 0; i < viewRows->size(); ++i) {
    const Row& row = (*viewRows)[i];
    fui::ListItem item;
    item.label = row.title.c_str();
    item.subtitle = row.detail.c_str();
    if (row.isNew) item.value = tr(STR_METHOD_NEW);
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;                 // a long title is cut to one line
  props.rowHeight = method::MIN_ROW_HEIGHT_PX;  // the whole row is the target, and at least 9 mm tall
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);
  props.headerUnderline = false;
  syncListViewport(screen, props);
  if (statusFocused.load()) props.selectedIndex = -1;  // the highlight is on the status line
  noteBuiltLayout(props.topIndex);
  screen.list(props);
}

// Render task. If the frame being built lays rows out differently from the one on the panel, taps are held back until
// it has been displayed (see frameOnPanel). The same layout needs no hold: a tap names the same row in both.
void MethodActivity::noteBuiltLayout(const int top) {
  builtLayout = {viewRows.get(), bandHeight, top};
  if (!(builtLayout == displayedLayout)) frameOnPanel.store(false);
}

void MethodActivity::drawFooter() {
  const bool hasRows = viewRows && !viewRows->empty();
  const bool focused = statusFocused.load();
  if (hasRows && !focused) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const int y = renderer.getScreenHeight() - metrics.buttonHintsHeight - lineHeight;
    GUI.drawHelpText(renderer, Rect{SIDE_PADDING, y, renderer.getScreenWidth() - 2 * SIDE_PADDING, lineHeight},
                     tr(STR_HOLD_OPEN_TO_REMOVE));
  }
  const char* confirm =
      focused ? (isBusy(viewStatus.kind) ? "" : tr(STR_METHOD_CHECK_NOW)) : (hasRows ? tr(STR_OPEN) : "");
  const auto labels = mappedInput.mapLabels(tr(STR_HOME), confirm, tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
