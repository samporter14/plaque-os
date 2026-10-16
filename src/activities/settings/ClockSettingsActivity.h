#pragma once
#include <string>

#include "NetChoiceDialog.h"
#include "activities/UiListActivity.h"

// Clock configuration under System settings: timezone, 12/24-hour format,
// home-header display, manual NTP sync, and "Reset the clock check" (forgets the floor and the sync record the reader
// holds its clock to, for a reader that shows a date in the future and cannot connect; asked first, the safe choice is
// the highlighted one; by touch and by every key). Only reachable when halClock.isAvailable() — SettingsActivity gates
// the entry.
class ClockSettingsActivity final : public UiListActivity {
 public:
  explicit ClockSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int ITEM_COUNT = 6;

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  int listCount() const override { return ITEM_COUNT; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;
  void promptReset();

  NetChoiceDialog resetDialog;
  bool resetDone = false;  // the row says so until the screen is left
  struct Finger {
    int x = 0;
    int y = 0;
    uint32_t at = 0;
    bool valid = false;
  } finger;

  // Row values are flash/translation strings assigned directly into rowItems_;
  // the one formatted value (the sync row's live time) lives here so its
  // pointer stays valid across the frame. Fits "HH:MM PM" + NUL.
  char syncTime_[9] = {0};
  freeink::ui::ListItem rowItems_[ITEM_COUNT]{};
};
