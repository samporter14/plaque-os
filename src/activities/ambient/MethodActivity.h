#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "MethodDevice.h"
#include "MethodRemoveDialog.h"
#include "activities/UiListActivity.h"
#include "ambient/MethodCheck.h"
#include "ambient/MethodFormat.h"
#include "ambient/MethodLibrary.h"

// Home item "Method": the shelf of books on the card (methods, tables, plate maps, boards). It opens from local
// state with no network, then checks the mailbox by itself (a step machine stepped from loop(); see
// MethodCheck). Opening any book, Back, Home and sleep cancel the check cleanly before the next screen takes
// over. Reads the Messages connection settings and never writes them.
//
// Every action works by touch and by keys (see FIRMWARE-UX.md, "Input"):
//   open a book          tap the row                        Up/Down to select, Confirm
//   remove a book        long-press the row                 hold Confirm on the selected row
//   check again now      tap the status line                Up from the first row to the status line, Confirm
//   back out             header back arrow or back-swipe    Back key; the Home key goes Home
// The remove dialog offers Keep and Remove as separate large choices; a long-press by touch puts Keep under
// the finger. Touch on a row is resolved against the frame that was drawn, so a new book sliding in cannot
// change which book a tap opens.
//
// Threading: loop(), the check and the library live on the main loop task. The render task only sees
// immutable snapshots (the row list and the status) published under viewMutex, so the loop never waits for
// a screen refresh.
class MethodActivity final : public UiListActivity {
  struct Row {
    std::string title;
    std::string detail;  // "7 steps · received today 21:00 · not started"
    std::string path;
    bool isNew = false;
  };
  using Rows = std::vector<Row>;

  enum class StatusKind : uint8_t {
    None,
    Joining,
    SettingClock,
    Checking,
    Downloading,
    Verifying,
    UpToDate,
    NewMethod,
    NoWifi,
    Auth,
    Server,
    Memory,
    SdCard,
    DownloadFailed,
    VerifyFailed,
    LowSpace,
    LowBattery,
    NotSetUp,
    Unreadable
  };
  struct Status {
    StatusKind kind = StatusKind::None;
    uint8_t percent = 0;
    uint8_t count = 0;  // NewMethod: books filed. Downloading: which download this is (1-based)
    uint8_t total = 0;  // Downloading: downloads in this check
    method::Kind bookKind = method::Kind::Method;
    char time[12] = {};    // "21:04"
    char needed[12] = {};  // "1.5" (megabytes)
  };
  static bool isBusy(StatusKind kind) { return kind >= StatusKind::Joining && kind <= StatusKind::Verifying; }

  struct Finger {
    int x = 0;
    int y = 0;
    uint32_t at = 0;
    bool valid = false;
  };

  static constexpr freeink::ui::ActionId ACTION_STATUS = ACTION_USER;

  // Declared in destruction order: the check goes before the ports it points at.
  std::string selectPath;
  std::unique_ptr<method::DevicePorts> ports;
  method::Library library;
  std::unique_ptr<method::Check> check;
  method::Words words;
  std::string kindNamesLower[4];

  // Main loop task only.
  std::shared_ptr<const Rows> rows;
  bool wantsCheck = false;
  bool leaving = false;
  bool userMovedSelection = false;
  bool lockNextConfirmRelease = false;
  int lastSelected = 0;
  uint32_t enteredAt = 0;
  uint32_t lastRevision = 0;
  Finger finger;

  // Shared with the render task.
  mutable std::mutex viewMutex;
  std::shared_ptr<const Rows> publishedRows;
  std::shared_ptr<const Rows> drawnRows;  // what the last render put on the panel: touch resolves rows against it
  // False from the moment a render builds a layout that differs from the one on the panel (other rows, status band
  // height or viewport) until that frame has been displayed. The framework publishes a frame's hit rectangles
  // while it builds it, up to a refresh before the user can see it, and a tap in between would name a row of a frame
  // that is not there yet: it is dropped, not guessed.
  std::atomic<bool> frameOnPanel{false};
  Status publishedStatus;
  std::atomic<int> rowCount{0};
  std::atomic<bool> rendered{false};
  std::atomic<bool> statusFocused{false};  // key focus is on the status line, above the first row

  // Render task only.
  std::shared_ptr<const Rows> viewRows;
  Status viewStatus;
  struct Layout {
    const Rows* rows = nullptr;
    int bandHeight = -1;
    int top = -1;
    bool operator==(const Layout& other) const {
      return rows == other.rows && bandHeight == other.bandHeight && top == other.top;
    }
  };
  Layout builtLayout;      // what the frame being built looks like
  Layout displayedLayout;  // what the frame on the panel looks like
  std::vector<freeink::ui::ListItem> items;
  std::vector<std::string> bandLines;
  int bandHeight = 0;

  MethodRemoveDialog removePopup;

  void fillWords();
  void rebuildRows();
  int indexOfPath(const std::string& path) const;
  std::string selectedPath() const;
  std::shared_ptr<const Rows> renderedRows() const;
  void trackSelection();
  void sampleFinger();
  void focusStatus(bool focused);

  void decideCheck();
  void maybeStartCheck();
  void startCheck();
  void recheck();
  void stepCheck();
  void finishCheck();
  void showFiledBooks();
  void cancelCheck();
  Status statusFromCheck() const;
  std::string timeText(int64_t epoch) const;
  void publishStatus(const Status& status);
  std::string statusText(const Status& status) const;
  void layoutStatusBand();
  void noteBuiltLayout(int top);

  void openPath(const std::string& path);
  void promptRemove(const std::string& path, const std::string& title, bool byTouch);
  void removeMethod(const std::string& path);
  static void statusTrampoline(const freeink::ui::ActionEvent& event, void* user);

 protected:
  int listCount() const override { return rowCount.load(); }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  void onRowAction(const freeink::ui::ActionEvent& event) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  void onBackButton() override;
  void drawChrome() override;
  void drawFooter() override;

 public:
  MethodActivity(GfxRenderer& renderer, MappedInputManager& input, std::string selectPath = {});
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool preventAutoSleep() override { return check && check->running(); }
  // The power button sleeps the device even with a check running. enterDeepSleep() calls this before the sleep-time
  // plugin events, which would otherwise find the check's Wi-Fi up and drain over its connection, and before the
  // loop can step the check once more (possibly into a TLS open). Tearing it down here puts the radio, the
  // connection and the partial file away first, exactly as leaving the shelf does.
  void prepareForSleep() override {
    leaving = true;
    cancelCheck();
  }
  bool handleHomeGesture() override;
};
