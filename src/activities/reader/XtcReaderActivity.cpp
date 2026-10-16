#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "MethodIndexRead.h"
#include "ProgressFile.h"
#include "ReaderActivity.h"
#include "ReaderUtils.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "ambient/StepTimerService.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
using StepRect = steptimer::Rect;

StepRect boxOf(const method::Timer& t) { return StepRect{t.x, t.y, t.w, t.h}; }

std::string withClock(const char* pattern, uint32_t seconds) {
  char out[96];
  snprintf(out, sizeof(out), pattern, steptimer::clockText(seconds).c_str());
  return out;
}
}  // namespace

bool XtcReaderActivity::loadBook() {
  auto loadedXtc = makeUniqueNoThrow<Xtc>(bookPath, "/.crosspoint");
  if (!loadedXtc) {
    LOG_ERR("XTR", "Failed to allocate XTC object");
    return false;
  }
  if (!loadedXtc->load()) {
    LOG_ERR("XTR", "Failed to load XTC");
    return false;
  }
  xtc = std::move(loadedXtc);
  xtc->setupCacheDir();
  loadProgress();
  loadTimers();
  return true;
}

void XtcReaderActivity::openChapterSelection() {
  if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
    childOpen_ = true;
    startActivityForResult(std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
                           [this](const ActivityResult& result) {
                             {
                               // The list drew over the page: whatever comes next draws the whole page again.
                               RenderLock lock(*this);
                               childOpen_ = false;
                               frameHasPage_ = false;
                             }
                             if (!result.isCancelled) {
                               currentPage = std::get<PageResult>(result.data).page;
                               requestUpdate();
                             }
                           });
  }
}

bool XtcReaderActivity::handleFormatInput() {
  if (!xtc) {
    return false;
  }

  // Confirm opens the Steps list, or, on a Method page with a timer or while one runs, a small menu that has the list
  // in it. The touch menu gesture goes straight to the list, as it always did.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (timerRelevant()) {
      openTimerMenu();
    } else {
      openChapterSelection();
    }
    return true;
  }
  if (ReaderUtils::isTouchMenuGesture(renderer, mappedInput)) {
    openChapterSelection();
    return true;
  }
  return false;
}

void XtcReaderActivity::applyInitialOrientation() { renderer.setOrientation(GfxRenderer::Orientation::Portrait); }

void XtcReaderActivity::renderBook() {
  if (!xtc) {
    return;
  }

  // A full page: the frame holds exactly this, with no question over it and no timer tick waiting.
  tickPending_ = false;
  dialogDrawn_ = false;
  renderPage();
  frameHasPage_ = true;
  renderedPage_ = currentPage;
  saveProgress();
}

XtcReaderActivity::StatusBarInfo XtcReaderActivity::getStatusBarInfo() const {
  const auto sb = SETTINGS.statusBarSpec();
  const int bookPageCount = static_cast<int>(xtc->getPageCount());
  const int bookPage = static_cast<int>(currentPage) + 1;
  std::string title = sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::BOOK_TITLE ? xtc->getTitle() : "";

  if (!xtc->hasChapters()) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  const auto& chapters = xtc->getChapters();
  const auto chapterIt = std::find_if(chapters.begin(), chapters.end(), [this](const xtc::ChapterInfo& chapter) {
    return currentPage >= chapter.startPage && currentPage <= chapter.endPage;
  });

  if (chapterIt == chapters.end() || chapterIt->endPage < chapterIt->startPage) {
    return StatusBarInfo{bookPage, bookPageCount, std::move(title)};
  }

  if (sb.titleMode == CrossPointSettings::STATUS_BAR_TITLE::CHAPTER_TITLE) {
    title = chapterIt->name.empty() ? tr(STR_UNNAMED) : chapterIt->name;
  }

  return StatusBarInfo{static_cast<int>(currentPage - chapterIt->startPage) + 1,
                       static_cast<int>(chapterIt->endPage - chapterIt->startPage) + 1, std::move(title)};
}

void XtcReaderActivity::renderStatusBarOverlay(GfxRenderer& renderer, const StatusBarOverlayPosition position) const {
  const auto sb = SETTINGS.statusBarSpec();
  const bool drawBottom = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_BOTTOM &&
                          position == StatusBarOverlayPosition::Bottom;
  const bool drawTop = sb.xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP &&
                       position == StatusBarOverlayPosition::Top;
  if (!drawBottom && !drawTop) {
    return;
  }

  const int statusBarHeight = UITheme::getInstance().getStatusBarHeight();
  if (statusBarHeight <= 0) {
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                   &orientedMarginLeft);

  int clearY;
  int paddingBottom = 0;
  if (position == StatusBarOverlayPosition::Bottom) {
    clearY = renderer.getScreenHeight() - orientedMarginBottom - statusBarHeight - 4;
    if (clearY < 0) {
      clearY = 0;
    }
  } else {
    clearY = orientedMarginTop;
    paddingBottom = renderer.getScreenHeight() - statusBarHeight - orientedMarginBottom - orientedMarginTop - 4;
  }
  const int clearHeight = position == StatusBarOverlayPosition::Bottom
                              ? renderer.getScreenHeight() - orientedMarginBottom - clearY
                              : statusBarHeight + 4;
  if (clearHeight > 0) {
    renderer.fillRect(0, clearY, renderer.getScreenWidth(), clearHeight, false);
  }

  const int pageCount = static_cast<int>(xtc->getPageCount());
  const int displayPage = static_cast<int>(currentPage) + 1;
  const float progress = pageCount > 0 ? (static_cast<float>(displayPage) * 100.0f) / pageCount : 0.0f;
  const auto pageInfo = getStatusBarInfo();
  GUI.drawStatusBar(renderer, progress, pageInfo.currentPage, pageInfo.pageCount, pageInfo.title, paddingBottom);
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = static_cast<size_t>(pageWidth) * ((static_cast<size_t>(pageHeight) + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu: bufferSize=%lu bitDepth=%u error=%s", currentPage, pageBufferSize,
            bitDepth, xtc::errorToString(xtc->getLastError()));
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderer.clearScreen();

  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    const size_t planeSize = static_cast<size_t>(pageWidth) * ((static_cast<size_t>(pageHeight) + 7) / 8);
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + planeSize;
    const size_t colBytes = (pageHeight + 7) / 8;

    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    if (pagesUntilFullRefresh <= 1) {
      // Periodic ghost cleanup: scrub via the normal path, then run the
      // settle flavor of the grayscale base pass (DTM planes are equal after
      // the display sync, so only the gentle reinforcement cells fire).
      // Combined-base panels (Paper Mono) instead defer the base so the gray
      // planes below join it in one waveform.
      if (renderer.grayscaleCapabilities().base == HalDisplay::GrayscaleBase::Combined) {
        renderer.displayGrayscaleBase(HalDisplay::HALF_REFRESH);
      } else {
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        renderer.preconditionGrayscale();
      }
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayGrayscaleBase(HalDisplay::FAST_REFRESH);
      pagesUntilFullRefresh--;
    }

    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();

    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit grayscale)", currentPage + 1, xtc->getPageCount());
    markPageRendered();
    return;
  } else {
    const size_t srcRowBytes = (pageWidth + 7) / 8;

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }

  free(pageBuffer);

  if (SETTINGS.statusBarSpec().xtcMode == CrossPointSettings::XTC_STATUS_BAR_MODE::XTC_STATUS_BAR_TOP) {
    renderStatusBarOverlay(renderer, StatusBarOverlayPosition::Top);
  } else {
    renderStatusBarOverlay(renderer, StatusBarOverlayPosition::Bottom);
  }

  // The running step timer goes on last, so it is part of this refresh and a page turn never shows it a minute late.
  drawTimerOverlay(steptimer::view::Mood::Running);

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
  markPageRendered();
}

bool XtcReaderActivity::pageTurn(bool isForward) {
  if (!xtc) return false;
  if (isForward) {
    if (currentPage < xtc->getPageCount()) {
      currentPage++;
      return true;
    }
  } else {
    if (currentPage > 0) {
      currentPage--;
      return true;
    }
  }
  return false;
}

bool XtcReaderActivity::skipPages(int amount) {
  if (!xtc) return false;
  int newPage = static_cast<int>(currentPage) + amount;
  if (newPage < 0) newPage = 0;
  if (newPage > static_cast<int>(xtc->getPageCount())) newPage = static_cast<int>(xtc->getPageCount());
  if (newPage != static_cast<int>(currentPage)) {
    currentPage = static_cast<uint32_t>(newPage);
    return true;
  }
  return false;
}

bool XtcReaderActivity::isAtEndOfBook() const { return xtc && currentPage >= xtc->getPageCount(); }

void XtcReaderActivity::onReturnFromEndOfBook() {
  if (xtc && xtc->getPageCount() > 0) {
    currentPage = xtc->getPageCount() - 1;
  } else {
    currentPage = 0;
  }
}

void XtcReaderActivity::saveProgress() const {
  if (!xtc) return;
  uint8_t data[4];
  data[0] = currentPage & 0xFF;
  data[1] = (currentPage >> 8) & 0xFF;
  data[2] = (currentPage >> 16) & 0xFF;
  data[3] = (currentPage >> 24) & 0xFF;
  if (!ProgressFile::writeAtomic(xtc->getCachePath(), data, sizeof(data))) {
    LOG_ERR("XTC", "Failed to save progress: page %lu", currentPage);
  }
}

void XtcReaderActivity::loadProgress() {
  if (!xtc) return;
  HalFile f;
  if (Storage.openFileForRead("XTC", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      if (currentPage >= xtc->getPageCount() && xtc->getPageCount() > 0) {
        currentPage = xtc->getPageCount() - 1;
      }
      LOG_DBG("XTC", "Loaded progress: page %lu/%lu", currentPage + 1, xtc->getPageCount());
    }
  }
}

ScreenshotInfo XtcReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Xtc;
  if (xtc) {
    const std::string t = xtc->getTitle();
    snprintf(info.title, sizeof(info.title), "%s", t.c_str());
    const uint32_t pageCount = xtc->getPageCount();
    info.totalPages = pageCount;
    uint32_t clampedPage = (pageCount > 0 && currentPage >= pageCount) ? pageCount - 1 : currentPage;
    info.progressPercent = pageCount > 0 ? xtc->calculateProgress(clampedPage) : 0;
    info.currentPage = static_cast<int>(clampedPage) + 1;
  } else {
    info.currentPage = currentPage + 1;
  }
  return info;
}

// ---- Step timers
// ------------------------------------------------------------------------------------------------------

void XtcReaderActivity::loadTimers() {
  timers_.clear();
  methodBook_ = method::ownedBookPath(bookPath);
  if (!methodBook_ || !xtc) return;
  method::Entry entry;
  if (!method::readIndexedEntry(bookPath, entry)) return;
  const uint32_t pages = xtc->getPageCount();
  for (const method::Timer& timer : entry.timers)
    if (timer.page >= 1 && timer.page <= pages) timers_.push_back(timer);
  if (!timers_.empty())
    LOG_INF("XTR", "%u step timer(s) in %s", static_cast<unsigned>(timers_.size()), bookPath.c_str());
}

const method::Timer* XtcReaderActivity::timerHere() const {
  return methodBook_ ? method::timerOnPage(timers_, currentPage + 1) : nullptr;
}

// Confirm means something other than the Steps list on a page with a timer, and on any page while one runs.
bool XtcReaderActivity::timerRelevant() const { return methodBook_ && (timerHere() || STEP_TIMER.running()); }

// Main task: the panel may no longer hold this page as it was drawn, so the next render draws all of it.
void XtcReaderActivity::needRepaint() {
  RenderLock lock(*this);
  frameHasPage_ = false;
}

// Render task (or, for the sleep frame, the main task under the lock).
void XtcReaderActivity::drawTimerOverlay(const steptimer::view::Mood mood) { drawTimerOverlay(mood, currentPage); }

void XtcReaderActivity::drawTimerOverlay(steptimer::view::Mood mood, const uint32_t shownPage) {
  if (!methodBook_) return;
  const steptimer::Record& rec = STEP_TIMER.record();
  if (rec.phase != steptimer::Phase::Running) {
    shownSeconds_.store(-1);
    // A timer that ended and was answered leaves its box saying so until the page changes.
    const steptimer::DoneNote& note = STEP_TIMER.done();
    if (note.page == 0) return;
    if (note.bookPath != bookPath || note.page != shownPage + 1) {
      STEP_TIMER.clearDone();
      return;
    }
    if (steptimer::layoutBox(note.box).digits.h >= 20 && steptimer::layoutBox(note.box).digits.w >= 80)
      steptimer::view::drawBox(renderer, note.box, steptimer::view::contentForDone(note.label, note.endedAt), false);
    return;
  }
  const int64_t remaining = STEP_TIMER.remainingMs();
  shownSeconds_.store(static_cast<int32_t>(steptimer::displaySeconds(remaining)));
  // Once the reader is on its way to sleep, a render that was still queued (a page turn, a tick) must not put a running
  // countdown back on a panel that is about to keep its frame.
  if (sleeping_) mood = steptimer::view::Mood::Sleeping;
  const steptimer::view::Content content = steptimer::view::contentFor(rec, mood, remaining);
  const bool onItsPage = rec.bookPath == bookPath && rec.page == shownPage + 1;
  if (onItsPage && steptimer::view::boxCanShow(rec)) {
    steptimer::view::drawBox(renderer, StepRect{rec.boxX, rec.boxY, rec.boxW, rec.boxH}, content, false);
  } else {
    steptimer::view::drawChip(renderer, content, false);
  }
}

void XtcReaderActivity::prepareForSleep() {
  ReaderActivity::prepareForSleep();
  // The panel will keep whatever is on it for as long as the reader sleeps, so a countdown must not be left frozen on
  // it: the timer says when it ends instead. Drawn into the frame only; the sleep screen's refresh sends it with the
  // moon. (Under the RenderLock, on the main task.)
  if (!xtc || !methodBook_ || !STEP_TIMER.running()) return;
  // From here every draw of the timer is the sleeping line, and no question is drawn again: a render still queued when
  // the reader sleeps (a page turn, a tick, a question being opened) would otherwise put the running countdown, or the
  // question over it, back on top of what is drawn below. A question that was open stays in the frame as it was drawn;
  // it cannot be taken out of a frame without drawing the page again, and the wake draws a page of its own.
  sleeping_ = true;
  dialog_.dismissLocked();  // ActivityManager::prepareForSleep() holds the lock, and it is not recursive
  const steptimer::Record& rec = STEP_TIMER.record();
  // What the frame holds is the page the last render drew, which a page turn still waiting has not caught up with.
  const bool frameShowsTimersPage = rec.bookPath == bookPath && rec.page == renderedPage_ + 1;
  if (steptimer::sleepOverlay(true, childOpen_, frameHasPage_, frameShowsTimersPage,
                              steptimer::view::boxCanShow(rec)) == steptimer::SleepOverlay::None)
    return;
  drawTimerOverlay(steptimer::view::Mood::Sleeping, renderedPage_);
}

// Main task. Redraws the timer, and only the timer, when the display is due to change.
void XtcReaderActivity::tickTimer() {
  if (!STEP_TIMER.running() || childOpen_) return;
  const int64_t remaining = STEP_TIMER.remainingMs();
  if (steptimer::timerDue(remaining)) return;  // the end belongs to the alert, which main.cpp starts
  const int32_t shown = shownSeconds_.load();
  if (shown >= 0 && !steptimer::tickDue(shown, remaining)) return;
  {
    // Never wait for the render task here (a page render takes a second and the keys are read on this task): try again
    // next pass.
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) return;
    if (frameHasPage_) tickPending_ = true;
  }
  shownSeconds_.store(static_cast<int32_t>(steptimer::displaySeconds(remaining)));
  requestUpdate();
}

void XtcReaderActivity::loop() {
  // Stamped on the way out, however loop() returns: the gap that matters is between one pass ending and the next
  // beginning, not how long a pass took (an NVS write or a wait for the render lock inside one is not another screen).
  struct Stamp {
    uint32_t& slot;
    ~Stamp() { slot = millis(); }
  } stamp{lastLoopEndMs_};

  if (xtc && methodBook_) {
    // This reader's loop does not run while another screen is on top (the Steps list, the light panel, an alert).
    // Coming back after a gap, the panel holds that screen, so the next draw is the whole page, never just the timer.
    if (lastLoopEndMs_ != 0 && millis() - lastLoopEndMs_ > 200) {
      needRepaint();
      dialog_.dismiss();  // a question that was open when another screen took over is stale
    }

    if (dialog_.isActive()) {
      dialog_.handleInput(mappedInput, [this] { requestUpdate(); });
      return;
    }
    if (handleTimerTap()) return;
    tickTimer();
  }
  ReaderActivity::loop();
}

void XtcReaderActivity::render(RenderLock&& lock) {
  if (xtc && methodBook_ && !isAtEndOfBook()) {
    // A question over the page, or the timer alone: neither is a page render, so the reading session and the refresh
    // cadence (a full refresh every N pages) are left as they are. Both draw over the page, so both need it to be
    // there.
    if (dialog_.isActive() && frameHasPage_ && renderedPage_ == currentPage) {
      dialog_.processRender(renderer, mappedInput);
      dialogDrawn_ = true;
      return;
    }
    if (!dialog_.isActive() && tickPending_ && frameHasPage_ && !dialogDrawn_ && renderedPage_ == currentPage) {
      tickPending_ = false;
      drawTimerOverlay(steptimer::view::Mood::Running);
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }
  ReaderActivity::render(std::move(lock));
  // A question opened while a repaint was still due is drawn now, over the page this render just drew, rather than left
  // open and invisible until the next render.
  if (xtc && methodBook_ && !isAtEndOfBook() && dialog_.isActive() && frameHasPage_ && renderedPage_ == currentPage) {
    dialog_.processRender(renderer, mappedInput);
    dialogDrawn_ = true;
  }
}

// A tap on a timer: its box on this page, or the chip. Returns true when it was one (the tap is then not a page turn).
bool XtcReaderActivity::handleTimerTap() {
  if (!mappedInput.hasTouch()) return false;
  int x = 0, y = 0;
  if (!mappedInput.wasScreenTapped(x, y)) return false;

  const steptimer::Record& rec = STEP_TIMER.record();
  const bool running = rec.phase == steptimer::Phase::Running;
  const method::Timer* here = timerHere();
  const bool hereRunning = running && here && rec.bookPath == bookPath && rec.page == here->page;
  // The chip is on screen on every page but the running timer's own, and on that one when its box is too small for the
  // clock.
  const bool chipShown = running && !(hereRunning && steptimer::view::boxCanShow(rec));
  if (chipShown && steptimer::touchTarget(steptimer::chipRect()).contains(x, y)) {
    askStop(true, x, y);
    return true;
  }
  if (here && steptimer::touchTarget(boxOf(*here)).contains(x, y)) {
    startTimer(*here, true, x, y);
    return true;
  }
  return false;
}

// Starts the page's timer, asking first when that would mean stopping another, and offering to stop when it is this
// one.
void XtcReaderActivity::startTimer(const method::Timer& timer, const bool byFinger, const int x, const int y) {
  switch (steptimer::decideStart(STEP_TIMER.record(), bookPath, timer.page)) {
    case steptimer::StartDecision::Start: {
      {
        RenderLock lock(*this);
        STEP_TIMER.start(bookPath, timer.page, timer.seconds, timer.label, boxOf(timer), currentPage + 1);
        if (frameHasPage_) tickPending_ = true;
      }
      shownSeconds_.store(static_cast<int32_t>(timer.seconds));
      requestUpdate();
      break;
    }
    case steptimer::StartDecision::AlreadyHere:
      askStop(byFinger, x, y);
      break;
    case steptimer::StartDecision::AskReplace:
      askReplace(timer, byFinger, x, y);
      break;
  }
}

void XtcReaderActivity::askStop(const bool byFinger, const int x, const int y) {
  const std::string title = withClock(tr(STR_METHOD_TIMER_STOP_TITLE), STEP_TIMER.record().seconds);
  dialog_.showPrompt(title.c_str(), nullptr, {tr(STR_METHOD_TIMER_KEEP), nullptr},
                     {tr(STR_METHOD_TIMER_STOP), [this] { stopTimer(); }}, byFinger, x, y);
  requestUpdate();
}

void XtcReaderActivity::askReplace(const method::Timer& timer, const bool byFinger, const int x, const int y) {
  char headline[160];
  snprintf(headline, sizeof(headline), tr(STR_METHOD_TIMER_REPLACE_HEAD),
           steptimer::clockText(STEP_TIMER.record().seconds).c_str(), steptimer::clockText(timer.seconds).c_str());
  dialog_.showPrompt(tr(STR_METHOD_TIMER_REPLACE_TITLE), headline, {tr(STR_METHOD_TIMER_KEEP), nullptr},
                     {tr(STR_METHOD_TIMER_REPLACE_ACT),
                      [this, timer] {
                        {
                          RenderLock lock(*this);
                          STEP_TIMER.start(bookPath, timer.page, timer.seconds, timer.label, boxOf(timer),
                                           currentPage + 1);
                          frameHasPage_ = false;
                        }
                        shownSeconds_.store(-1);
                      }},
                     byFinger, x, y);
  requestUpdate();
}

void XtcReaderActivity::stopTimer() {
  {
    RenderLock lock(*this);
    STEP_TIMER.clear();
    frameHasPage_ = false;  // the page's own box is back as the book printed it: draw the whole page
  }
  shownSeconds_.store(-1);
  requestUpdate();
}

// Confirm on a page with a timer, or while one runs: start or stop it, or open the Steps list. A key can do all that a
// tap can.
void XtcReaderActivity::openTimerMenu() {
  const steptimer::Record rec = STEP_TIMER.record();
  const bool running = rec.phase == steptimer::Phase::Running;
  const method::Timer* here = timerHere();
  const bool hereRunning = running && here && rec.bookPath == bookPath && rec.page == here->page;

  std::vector<StepTimerDialog::Choice> rows;
  if (here && !hereRunning)
    rows.push_back({withClock(tr(STR_METHOD_TIMER_START), here->seconds),
                    [this, timer = *here] { startTimer(timer, false, 0, 0); }});
  if (running)
    rows.push_back({withClock(tr(STR_METHOD_TIMER_STOP_ROW), rec.seconds), [this] { askStop(false, 0, 0); }});
  rows.push_back({tr(STR_METHOD_STEPS), [this] { openChapterSelection(); }});
  dialog_.showMenu(nullptr, std::move(rows));
  requestUpdate();
}
