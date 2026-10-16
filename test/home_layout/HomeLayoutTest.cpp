#include <cstdlib>
#include <iostream>
#include <string>

// The generated font header leaves a trailing field to default; that is its business, not this test's.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "EpdFont/builtinFonts/ubuntu_12_regular.h"
#pragma GCC diagnostic pop
#include "components/HomeMenuLayout.h"
#include "components/themes/BaseTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace {
int checks = 0;
void check(bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

constexpr int PAGE_HEIGHT = 800;  // portrait panel, 480 x 800
constexpr int HINT_BAND = 40;     // every theme's buttonHintsHeight, where the hints are drawn (non-touch)

struct Theme {
  const char* name;
  const ThemeMetrics& metrics;
  int preferredRow;     // the row height the theme draws when everything fits
  bool continueInMenu;  // RoundedRaff lists Continue Reading as the first menu row
};

// RoundedRaff derives its rows from the UI font: line height + 20 (RoundedRaffTheme::getMenuRowHeight).
const int ROUNDED_RAFF_ROW = ubuntu_12_regular.advanceY + 20;

const Theme THEMES[] = {
    {"Classic", BaseMetrics::values, BaseMetrics::values.menuRowHeight, false},
    {"Lyra", LyraMetrics::values, LyraMetrics::values.menuRowHeight, false},
    {"Lyra 3 covers", Lyra3CoversMetrics::values, Lyra3CoversMetrics::values.menuRowHeight, false},
    {"RoundedRaff", RoundedRaffMetrics::values, ROUNDED_RAFF_ROW, true},
};

int menuTop(const Theme& t) {
  return t.metrics.homeTopPadding + t.metrics.homeCoverTileHeight + t.metrics.homeMenuTopOffset;
}
int bottomOf(int top, int count, int height, int gap) { return top + count * height + (count - 1) * gap; }

// Rows the Home menu can hold: File Browser, Library, File Transfer, Settings, Method, plus the optional library
// slot (OPDS or Plugins) and, in RoundedRaff, Continue Reading.
int menuCount(const Theme& t, bool librarySlot, bool recents) {
  return 5 + (librarySlot ? 1 : 0) + (t.continueInMenu && recents ? 1 : 0);
}
}  // namespace

int main() {
  check(ubuntu_12_regular.advanceY == 29, "UI font line height the RoundedRaff rows are derived from");
  check(RoundedRaffMetrics::values.menuSpacing == 6,
        "RoundedRaff gap equals its kSelectableRowGap (touch grid uses menuSpacing)");

  for (const Theme& t : THEMES) {
    const int top = menuTop(t);
    const int gap = t.metrics.menuSpacing;
    for (const bool hintBand : {false, true}) {  // false: the X4 Pro, a touch board with no band
      const int hints = hintBand ? HINT_BAND : 0;
      const int limit = PAGE_HEIGHT - hints - homelayout::bottomMargin(hints);
      const int budget = homelayout::menuBudget(PAGE_HEIGHT, top, hints);
      check(budget == limit - top, std::string(t.name) + ": budget runs from the first row to the margin");
      for (const bool librarySlot : {false, true}) {
        for (const bool recents : {false, true}) {
          const int n = menuCount(t, librarySlot, recents);
          const auto rows = homelayout::fitRows(n, budget, t.preferredRow, gap);
          const std::string label =
              std::string(t.name) + (hintBand ? " with hint band" : " touch") + ", " + std::to_string(n) + " rows: ";
          check(bottomOf(top, n, rows.height, gap) <= limit, label + "last row clears the hint band / panel edge");
          check(rows.step == rows.height + gap, label + "step is height + gap");
          check(rows.height >= t.preferredRow * 3 / 4 && rows.height <= t.preferredRow, label + "row stays readable");
          check(top >= 0 && bottomOf(top, n, rows.height, gap) <= PAGE_HEIGHT, label + "on the panel");
          // A theme that already fit keeps its own size exactly.
          const bool fitted = bottomOf(top, n, t.preferredRow, gap) <= limit;
          check(!fitted || rows.height == t.preferredRow, label + "rows that fit are not touched");
          // The glove rule on touch boards: wherever a layout already fit on the panel before the squeeze existed
          // (the Method row and library slot included), its rows keep their natural size.
          if (!hintBand && bottomOf(top, n, t.preferredRow, gap) <= PAGE_HEIGHT) {
            check(rows.height == t.preferredRow, label + "touch rows that fit the panel are never squeezed");
          }
          // RoundedRaff pages its menu; every row must land on the first page, since the touch grid is not paged.
          check((budget + gap) / rows.step >= n, label + "one page");
        }
      }
    }
  }

  // Exact geometry for the cases the Method item and the library slot can produce (the numbers in the report).
  {
    const Theme& lyra3 = THEMES[2];
    const int top = menuTop(lyra3);
    check(top == 372, "Lyra 3 covers menu top");
    check(bottomOf(top, 6, 64, 8) == 796, "Lyra 3 covers, 6 rows unsqueezed: 36 px into the hint band");
    check(bottomOf(top, 6, 64, 8) > PAGE_HEIGHT - HINT_BAND, "...which the survey found");
    const auto touch = homelayout::fitRows(6, homelayout::menuBudget(PAGE_HEIGHT, top, 0), 64, 8);
    check(touch.height == 64 && bottomOf(top, 6, touch.height, 8) == 796,
          "Lyra 3 covers, 6 rows, touch: 64 px rows kept, ending 4 px above the edge as before");
    const auto band = homelayout::fitRows(6, homelayout::menuBudget(PAGE_HEIGHT, top, HINT_BAND), 64, 8);
    check(band.height == 56 && bottomOf(top, 6, band.height, 8) == 748, "Lyra 3 covers, 6 rows, band: 56 px rows");
    const auto five = homelayout::fitRows(5, homelayout::menuBudget(PAGE_HEIGHT, top, HINT_BAND), 64, 8);
    check(five.height == 64 && bottomOf(top, 5, 64, 8) == 724, "Lyra 3 covers, 5 rows: untouched");
  }
  {
    const Theme& raff = THEMES[3];
    const int top = menuTop(raff);
    check(top == 425 && ROUNDED_RAFF_ROW == 49, "RoundedRaff menu top and row height");
    check(bottomOf(top, 7, 49, 6) == 804, "RoundedRaff, 7 rows unsqueezed: off the 800 px panel");
    check(bottomOf(top, 6, 49, 6) == 749, "RoundedRaff, 6 rows unsqueezed: fits");
    const auto touch = homelayout::fitRows(7, homelayout::menuBudget(PAGE_HEIGHT, top, 0), 49, 6);
    check(touch.height == 47 && bottomOf(top, 7, touch.height, 6) == 790, "RoundedRaff, 7 rows, touch: 47 px rows");
    const auto band = homelayout::fitRows(7, homelayout::menuBudget(PAGE_HEIGHT, top, HINT_BAND), 49, 6);
    check(band.height == 41 && bottomOf(top, 7, band.height, 6) == 748, "RoundedRaff, 7 rows, band: 41 px rows");
  }
  {
    const Theme& classic = THEMES[0];
    const int top = menuTop(classic);
    check(top == 460, "Classic menu top equals where its rows have always been drawn (450 + verticalSpacing)");
    check(bottomOf(top, 6, 45, 8) == 770, "Classic, 6 rows unsqueezed: 10 px into the hint band");
    const auto band = homelayout::fitRows(6, homelayout::menuBudget(PAGE_HEIGHT, top, HINT_BAND), 45, 8);
    check(band.height == 42 && bottomOf(top, 6, band.height, 8) == 752, "Classic, 6 rows, band: 42 px rows");
    const auto touch = homelayout::fitRows(6, homelayout::menuBudget(PAGE_HEIGHT, top, 0), 45, 8);
    check(touch.height == 45, "Classic, 6 rows, touch: untouched");
  }
  {
    const Theme& lyra = THEMES[1];
    const int top = menuTop(lyra);
    check(top == 314 && bottomOf(top, 6, 64, 8) == 738, "Lyra, 6 rows: 738, clear of a 760 band");
    check(homelayout::fitRows(6, homelayout::menuBudget(PAGE_HEIGHT, top, HINT_BAND), 64, 8).height == 64,
          "Lyra, 6 rows, band: untouched");
  }

  // Touch boards keep Lyra's 64 px rows for every count Home can produce (the glove rule: rows >= 64 px).
  for (const Theme* t : {&THEMES[1], &THEMES[2]}) {
    for (int n = 4; n <= 6; ++n) {
      const auto rows = homelayout::fitRows(n, homelayout::menuBudget(PAGE_HEIGHT, menuTop(*t), 0), 64, 8);
      check(rows.height == 64, std::string(t->name) + ": touch rows stay 64 px");
    }
  }
  check(homelayout::bottomMargin(0) == 4 && homelayout::bottomMargin(HINT_BAND) == 8, "margins: 4 px edge, 8 px band");

  // Degenerate inputs never divide by zero or grow rows.
  check(homelayout::fitRows(0, 100, 40, 8).height == 40, "no rows");
  check(homelayout::fitRows(1, 10, 40, 8).height == 40, "one row is never squeezed");
  check(homelayout::fitRows(20, 100, 40, 8).height == 30, "never below three quarters of the row");

  std::cout << checks << " Home menu layout checks passed\n";
}
