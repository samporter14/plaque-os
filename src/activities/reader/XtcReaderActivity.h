#pragma once

#include <Xtc.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "ReaderActivity.h"
#include "StepTimerDialog.h"
#include "StepTimerView.h"
#include "ambient/MethodProtocol.h"

class XtcReaderActivity final : public ReaderActivity {
  std::shared_ptr<Xtc> xtc;
  uint32_t currentPage = 0;

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  void openChapterSelection();

  // ---- Step timers (Method books) ----
  // A Method book's page can carry a timer: tap its box (or choose it from Confirm's menu) to start; a running timer
  // draws its countdown over that box, and as a small chip on every other page. See ambient/StepTimer.h for the rules.
  std::vector<method::Timer> timers_;  // this book's timers, from the library index; empty for any other book
  bool methodBook_ = false;
  StepTimerDialog dialog_;  // the menu and the questions, drawn over the page
  bool childOpen_ = false;  // the Steps list is open above this reader (main task)
  uint32_t lastLoopEndMs_ =
      0;  // when loop() last returned: a long gap before the next call means another screen had the panel
  // Shared with the render task. The main task changes the first two only under the RenderLock; the render task reads
  // them.
  bool tickPending_ = false;  // redraw only the timer
  bool frameHasPage_ =
      false;  // the framebuffer, and so the panel, holds this reader's page as the last full render drew it
              // (the render task sets it; the main task clears it when anything else may have drawn)
  bool sleeping_ = false;  // the reader is on its way to sleep: whatever draws the timer now says when it ends
  bool dialogDrawn_ = false;               // a question was drawn over that page (render task)
  uint32_t renderedPage_ = UINT32_MAX;     // the page the last full render drew (render task)
  std::atomic<int32_t> shownSeconds_{-1};  // what the overlay last showed; the loop task decides from it when to redraw

  void loadTimers();
  const method::Timer* timerHere() const;
  bool timerRelevant() const;
  void drawTimerOverlay(steptimer::view::Mood mood);
  void drawTimerOverlay(steptimer::view::Mood mood, uint32_t shownPage);  // for the page the frame holds
  void tickTimer();
  bool handleTimerTap();
  void openTimerMenu();
  void startTimer(const method::Timer& timer, bool byFinger, int x, int y);
  void askStop(bool byFinger, int x, int y);
  void askReplace(const method::Timer& timer, bool byFinger, int x, int y);
  void stopTimer();
  void needRepaint();
  void renderStatusBarOverlay(GfxRenderer& renderer, StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

  bool loadBook() override;
  std::string getBookTitle() const override { return xtc ? xtc->getTitle() : ""; }
  std::string getBookAuthor() const override { return xtc ? xtc->getAuthor() : ""; }
  std::string getBookThumbBmpPath() const override { return xtc ? xtc->getThumbBmpPath() : ""; }
  bool handleFormatInput() override;
  void renderBook() override;
  void applyInitialOrientation() override;

 public:
  explicit XtcReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string bookPath,
                             bool allowFastInitialRefresh)
      : ReaderActivity("XtcReader", renderer, mappedInput, std::move(bookPath), allowFastInitialRefresh) {}
  ~XtcReaderActivity() override = default;

  void loop() override;
  void render(RenderLock&& lock) override;
  void prepareForSleep() override;

  bool pageTurn(bool isForward) override;
  bool skipPages(int amount) override;
  bool isAtEndOfBook() const override;
  void onReturnFromEndOfBook() override;

  ScreenshotInfo getScreenshotInfo() const override;
};
