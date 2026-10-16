#pragma once

// Where the reader's exits lead for a Method book. Header-only and free of Arduino, so the host tests
// (test/method_book) cover the decision that ReaderUtils::handleBackNavigation acts on.
namespace method {

// Does Back open the file browser? For any other book the stock rules apply: a long press opens the file browser
// and a short press goes Home, and Settings > Controls > "Back short press to file browser" swaps the two. A Method
// book ignores both the press length and that setting: Back always leaves through the reader's own "go home", which
// returns to the Method shelf with the book selected. The shelf is where a protocol is found again, and a file
// browser is no place to land with gloves on mid-incubation.
inline bool backOpensFileBrowser(const bool methodBook, const bool longPress, const bool backShortToFileBrowser) {
  return !methodBook && longPress != backShortToFileBrowser;
}

}  // namespace method
