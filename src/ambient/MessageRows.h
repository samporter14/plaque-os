#pragma once

#include <cstdint>

// The rows of Settings > System > Messages, and which of them a board shows. Pure, covered by test/beta_seams.
//
// The mailbox screen is on every board with the mailbox. The Register rows (view, check, and the sleep-screen choice)
// are Register's: they show only where the board offers it (plaque::offersRegister), so a board without it has the
// screen it had before Register existed, row for row. The rows keep one numbering (Item) however many are shown; the
// screen lists the shown ones in order and maps a position back to its Item.
namespace messagerows {

enum Item : uint8_t {
  VIEW,
  CHECK,
  REGISTER_VIEW,
  REGISTER_CHECK,
  URL,
  DEVICE_ID,
  TOKEN,
  INTERVAL,
  SLEEP_SCREEN,
  REGISTER_SLEEP,
  POLLING,
  LOCAL_HTTP,
  ITEM_COUNT
};

constexpr bool isRegisterRow(const Item item) {
  return item == REGISTER_VIEW || item == REGISTER_CHECK || item == REGISTER_SLEEP;
}

struct Rows {
  Item items[ITEM_COUNT] = {};
  int count = 0;
};

constexpr Rows rowsFor(const bool offersRegister) {
  Rows rows;
  for (int i = 0; i < ITEM_COUNT; ++i) {
    const Item item = static_cast<Item>(i);
    if (isRegisterRow(item) && !offersRegister) continue;
    rows.items[rows.count++] = item;
  }
  return rows;
}

}  // namespace messagerows
