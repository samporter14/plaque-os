#pragma once

// Row geometry of Home's button menu. The themes that draw the menu and HomeActivity's touch grid both take it
// from here, so the rows drawn and the rows tapped cannot disagree, and every row stays clear of the hint band
// (or the panel edge on touch boards). Pure arithmetic, no renderer: covered by test/home_layout.
namespace homelayout {

// Clear space kept between the last row and the hint band.
constexpr int BAND_MARGIN = 8;
// And between the last row and the panel edge when there is no band (touch boards). Smaller on purpose: rows that
// fit the panel must keep their natural size, because touch rows are never squeezed below the glove-friendly
// 64 px where they used to fit (Lyra 3 covers with the library slot and Method ends at 796).
constexpr int EDGE_MARGIN = 4;

struct Rows {
  int height;  // drawn row height
  int step;    // from one row's top to the next: the height plus the gap
};

// `count` rows of `height` with `gap` between them have to fit in `budget` pixels, measured from the first row's
// top to the last row's bottom. Rows keep the theme's own size whenever they already fit. Otherwise only the
// height shrinks, and never below three quarters of it so labels and icons keep their room.
constexpr Rows fitRows(const int count, const int budget, const int height, const int gap) {
  if (count > 1 && count * height + (count - 1) * gap > budget) {
    const int fitted = (budget - (count - 1) * gap) / count;
    const int smallest = height * 3 / 4;
    const int fittedHeight = fitted > smallest ? fitted : smallest;
    return {fittedHeight, fittedHeight + gap};
  }
  return {height, height + gap};
}

// Clear space below the last row: BAND_MARGIN above a hint band, EDGE_MARGIN above the panel edge without one.
constexpr int bottomMargin(const int hintsHeight) { return hintsHeight > 0 ? BAND_MARGIN : EDGE_MARGIN; }

// Room for the menu: from its first row's top down to bottomMargin() above the hint band (zero on touch boards).
constexpr int menuBudget(const int pageHeight, const int menuTop, const int hintsHeight) {
  return pageHeight - hintsHeight - bottomMargin(hintsHeight) - menuTop;
}

}  // namespace homelayout
