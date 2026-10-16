#pragma once

#include <cstdint>
#include <string>

#include "NetChoiceDialog.h"
#include "activities/UiListActivity.h"
#include "network/NetworkCardImport.h"
#include "network/NetworkProfile.h"

// Settings › System › Network: the reader's Wi-Fi address (the one Wi-Fi registration needs), its 2.4 GHz band, the
// installed network profile, and what can be done about it: Connect to Mac (USB setup), Import from card (the bundle is
// read and the file wiped, then the screen says what it would install and what it replaces, and nothing is installed
// until the person chooses Install; Back declines), Remove profile (asked first). Every row is at least 64 px and works
// by touch and by keys (Back leaves).
class NetworkActivity final : public UiListActivity {
 public:
  NetworkActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  enum class Row : uint8_t {
    Address,
    Band,
    Profile,
    Server,
    Expires,
    CheckConnection,
    ConnectMac,
    ImportCard,
    Remove,
  };
  static constexpr int MAX_ROWS = 10;

  netprofile::Summary profile;
  std::string address;
  std::string profileSubtitle;
  std::string expires;
  std::string importStatus;  // the last Import from card result, shown under its row
  std::string removeStatus;  // the last Remove result, shown under the profile row
  Row order[MAX_ROWS] = {};
  freeink::ui::ListItem rows[MAX_ROWS] = {};
  int rowCount = 0;
  NetChoiceDialog removeDialog;
  NetChoiceDialog importDialog;   // "Install this network profile?" for a bundle read from the card
  netprofile::CardRead cardRead;  // that bundle, validated, while the person decides (the file is already gone)
  struct Finger {
    int x = 0;
    int y = 0;
    uint32_t at = 0;
    bool valid = false;
  } finger;

  int listCount() const override { return rowCount; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;

  void reload();
  void rebuildRows();
  void promptRemove();
  void importFromCard();
  void finishImport(bool install);
  void openConnectToMac();
  void openCheckConnection();
};
