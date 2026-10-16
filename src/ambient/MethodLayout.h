#pragma once

#include <string>
#include <vector>

// Geometry and mapping for the Method shelf's touch behaviour. Pure logic with no Arduino or UI
// dependency, covered by test/method_layout, so the rules that keep a gloved finger from deleting a
// method are tested on the host.
namespace method {

struct Box {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
};

// Smallest sizes the bench needs (about 9 mm rows and 17 mm choices on the X4 Pro panel).
constexpr int MIN_ROW_HEIGHT_PX = 64;
constexpr int MIN_CHOICE_WIDTH_PX = 120;

// The "Remove from this reader?" dialog has two side-by-side choices. The dialog sits so that Keep is
// where the finger already is: a second touch at the same spot (a brush of the glove, a finger that has
// not lifted yet) can only ever choose Keep. Remove is on the other side and never under the finger.
struct RemoveDialogSpec {
  int screenW = 0;
  int screenH = 0;
  int dialogW = 0;    // wanted width; reduced to fit
  int dialogH = 0;    // height of the content, buttons included
  int padLeft = 0;    // inner padding, as passed to the dialog widget
  int padRight = 0;
  int padBottom = 0;
  int buttonH = 0;    // height of the choice row (at least MIN_ROW_HEIGHT_PX is the caller's duty)
  int gap = 0;        // between the two choices
  int margin = 0;     // minimum distance to every screen edge
};

struct RemoveDialogPlacement {
  Box dialog;
  Box keep;
  Box remove;
  bool keepFirst = true;       // Keep is the left choice
  bool keepUnderFinger = false;
};

// finger: where the long-press happened; pass hasFinger = false for a key press (centred, Keep first).
RemoveDialogPlacement placeRemoveDialog(const RemoveDialogSpec& spec, bool hasFinger, int fingerX, int fingerY);

// A tap names a row of the frame that was on screen (drawnPaths[drawnIndex]). If the shelf changed since
// (a new method slid in, one was removed), the same index is a different book now. Returns where the
// drawn row sits in `currentPaths`, or -1 when it is gone or the index is out of range.
int resolveDrawnRow(const std::vector<std::string>& drawnPaths, int drawnIndex,
                    const std::vector<std::string>& currentPaths);

// The shelf's focus ring: position 0 is the status line, 1..rowCount are the rows.
inline int ringForRow(int row) { return row + 1; }
inline int rowForRing(int ring) { return ring - 1; }  // -1: the status line is focused

}  // namespace method
