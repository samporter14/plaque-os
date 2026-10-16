#pragma once

#include "activities/UiListActivity.h"
#include "ambient/MessageRows.h"

class MessagesActivity final : public UiListActivity {
  static constexpr int ITEM_COUNT = messagerows::ITEM_COUNT;
  freeink::ui::ListItem rows[ITEM_COUNT] = {};   // by messagerows::Item
  freeink::ui::ListItem shown[ITEM_COUNT] = {};  // the rows this board shows, in order
  messagerows::Rows layout;                      // which Item each shown row is
  char interval[24] = {};
  char pollingText[64] = {};
  bool pollingNote = false;  // Register was just switched on and switched polling on with it

  int listCount() const override { return layout.count; }
  const char* headerTitle() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void editField(int item);

 public:
  MessagesActivity(GfxRenderer& renderer, MappedInputManager& input);
  void onEnter() override;
};
