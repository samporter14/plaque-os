#pragma once

// Moving about the awake Register screen with nothing but the two side buttons (Right is next, Left is previous) or a
// tap: one ring, so every place has a way in and a way out whatever is pressed.
//
//   screen -> page 1 -> page 2 -> ... -> the last page -> screen        (Right, a tap)
//   screen <- page 1 <- page 2 <- ... <- the last page <- screen        (Left)
//
// Pure (covered by test/register_layout). A position is 0 for the screen itself and 1..pages for a page of the full day;
// LAST_PAGE stands for the last page before the count of pages is known (the layout knows it).
namespace reg::nav {

constexpr int SCREEN = 0;
constexpr int LAST_PAGE = 1 << 20;

// `pages` is the count of pages the full day has, 0 when it is not known yet.
constexpr int next(const int position, const int pages) {
  if (position <= SCREEN) return 1;
  if (pages > 0 && position >= pages) return SCREEN;
  return position >= LAST_PAGE ? SCREEN : position + 1;
}

constexpr int previous(const int position, const int pages) {
  if (position <= SCREEN) return pages > 0 ? pages : LAST_PAGE;
  return position - 1;  // from page 1 that is the screen
}

// A position against the real count of pages: LAST_PAGE becomes the last page, and a position past the end is the screen.
constexpr int resolved(const int position, const int pages) {
  if (position <= SCREEN || pages <= 0) return SCREEN;
  if (position == LAST_PAGE) return pages;
  return position > pages ? SCREEN : position;
}

}  // namespace reg::nav
