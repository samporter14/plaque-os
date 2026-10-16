#pragma once
#include <string>

#include "activities/UiListActivity.h"

// Read-only device information: detected hardware (device profile, display
// controller, touch, frontlight, RTC, IMU), the firmware's name and version and
// the project it is based on, the reader's own UUID and mailbox device id, and
// the station Wi-Fi MAC address. A Plaque reader (X4 Pro, X4 Classic) also shows
// its model and labels the MAC as the address Wi-Fi registration needs. Rows are
// informational; activating one does nothing.
class AboutActivity final : public UiListActivity {
 public:
  explicit AboutActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  static constexpr int MAX_ITEMS = 15;

  void onEnter() override;

 private:
  int listCount() const override { return count_; }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int) override {}
  const char* headerTitle() const override;
  void addRow(const char* label, std::string value, const char* subtitle = nullptr);

  std::string rowValues_[MAX_ITEMS];
  std::string readerUuid_;  // the second line of the Reader ID row
  freeink::ui::ListItem rowItems_[MAX_ITEMS]{};
  int count_ = 0;
};
