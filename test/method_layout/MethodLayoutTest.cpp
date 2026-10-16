#include <iostream>

#include "FakePorts.h"
#include "MethodLayout.h"

using fake::check;
using fake::checkEq;
using method::Box;

namespace {
// The numbers the shelf uses on the 480 x 800 X4 Pro panel.
method::RemoveDialogSpec panelSpec() {
  method::RemoveDialogSpec spec;
  spec.screenW = 480;
  spec.screenH = 800;
  spec.dialogW = 400;
  spec.dialogH = 300;
  spec.padLeft = 16;
  spec.padRight = 16;
  spec.padBottom = 16;
  spec.buttonH = 72;
  spec.gap = 16;
  spec.margin = 16;
  return spec;
}

bool overlaps(const Box& a, const Box& b) {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

bool inside(const Box& inner, const Box& outer) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}

void testBox() {
  const Box box{10, 20, 100, 50};
  check(box.contains(10, 20) && box.contains(109, 69), "corners inside");
  check(!box.contains(110, 20) && !box.contains(10, 70) && !box.contains(9, 20) && !box.contains(10, 19), "edges outside");
}

void testNoFinger() {
  const auto spec = panelSpec();
  const auto p = method::placeRemoveDialog(spec, false, 0, 0);
  checkEq(p.dialog.x, 40, "centred horizontally");
  checkEq(p.dialog.y, 250, "centred vertically");
  check(p.keepFirst, "Keep is the first choice when a key opened the dialog");
  check(p.keep.x < p.remove.x, "Keep on the left");
  check(!p.keepUnderFinger, "no finger, no claim");
}

void testSizes() {
  const auto spec = panelSpec();
  for (int y = 0; y < spec.screenH; y += 25) {
    for (int x = 0; x < spec.screenW; x += 20) {
      const auto p = method::placeRemoveDialog(spec, true, x, y);
      check(p.keep.w >= method::MIN_CHOICE_WIDTH_PX && p.remove.w >= method::MIN_CHOICE_WIDTH_PX, "choices are wide enough");
      check(p.keep.h >= method::MIN_ROW_HEIGHT_PX && p.remove.h >= method::MIN_ROW_HEIGHT_PX, "choices are tall enough");
      check(inside(p.keep, p.dialog) && inside(p.remove, p.dialog), "choices sit inside the dialog");
      check(!overlaps(p.keep, p.remove), "choices do not overlap");
      check(p.dialog.x >= spec.margin && p.dialog.y >= spec.margin && p.dialog.right() <= spec.screenW - spec.margin &&
                p.dialog.bottom() <= spec.screenH - spec.margin,
            "dialog stays on the screen");
    }
  }
}

void testFingerNeverOnRemove() {
  const auto spec = panelSpec();
  int keepUnder = 0;
  int total = 0;
  for (int y = 0; y < spec.screenH; y += 7) {
    for (int x = 0; x < spec.screenW; x += 7) {
      const auto p = method::placeRemoveDialog(spec, true, x, y);
      ++total;
      if (p.remove.contains(x, y)) {
        std::cerr << "FAIL: finger on Remove at " << x << "," << y << '\n';
        std::exit(1);
      }
      fake::checkCount()++;
      checkEq(p.keepUnderFinger, p.keep.contains(x, y), "keepUnderFinger reports the truth");
      keepUnder += p.keepUnderFinger;
    }
  }
  check(keepUnder * 2 > total, "for most of the screen a repeated touch can only land on Keep");
}

void testFingerInTheMiddleIsOnKeep() {
  const auto spec = panelSpec();
  // The choice row sits rowY below the top of the dialog; the dialog stays on the screen. So Keep can be
  // put under any finger between these two heights (higher up, the dialog cannot move far enough).
  const int rowY = spec.dialogH - spec.padBottom - spec.buttonH;
  const int lowest = spec.margin + rowY;
  const int highest = spec.screenH - spec.margin - spec.dialogH + rowY + spec.buttonH - 1;
  for (int y = lowest; y <= highest; y += 10) {
    for (int x = 90; x <= 390; x += 10) {
      const auto p = method::placeRemoveDialog(spec, true, x, y);
      check(p.keepUnderFinger, "a long-press away from the edges leaves Keep under the finger");
    }
  }
  // Too high for that: the dialog sits as high as it can, and Remove is still not under the finger.
  const auto high = method::placeRemoveDialog(spec, true, 240, 120);
  check(!high.keepUnderFinger && !high.remove.contains(240, 120) && high.dialog.y == spec.margin,
        "a finger above the reachable band still never lands on Remove");
  // A long-press on the right half puts Keep on the right, so the finger's side is the safe side.
  const auto right = method::placeRemoveDialog(spec, true, 400, 400);
  check(!right.keepFirst && right.keepUnderFinger, "right-hand finger: Keep is the right choice");
  const auto left = method::placeRemoveDialog(spec, true, 80, 400);
  check(left.keepFirst && left.keepUnderFinger, "left-hand finger: Keep is the left choice");
  const auto centre = method::placeRemoveDialog(spec, true, 240, 400);
  check(!centre.remove.contains(240, 400), "dead centre is never Remove");
}

void testNarrowScreens() {
  auto spec = panelSpec();
  spec.screenW = 360;
  spec.dialogW = 400;  // wider than the screen: it shrinks to fit
  const auto p = method::placeRemoveDialog(spec, true, 180, 400);
  check(p.dialog.w <= 360 - 2 * spec.margin, "dialog shrinks to the screen");
  check(p.keep.w >= method::MIN_CHOICE_WIDTH_PX, "choices are still wide enough at 360 px");
  check(!p.remove.contains(180, 400), "finger not on Remove");
}

void testResolveDrawnRow() {
  const std::vector<std::string> drawn = {"/a.xtc", "/b.xtc", "/c.xtc"};
  checkEq(method::resolveDrawnRow(drawn, 1, drawn), 1, "unchanged shelf");
  // A new method arrived on top while the old frame was still on screen.
  const std::vector<std::string> arrived = {"/new.xtc", "/a.xtc", "/b.xtc", "/c.xtc"};
  checkEq(method::resolveDrawnRow(drawn, 1, arrived), 2, "a tap on the drawn row 1 still means /b.xtc");
  checkEq(method::resolveDrawnRow(drawn, 0, arrived), 1, "and the drawn row 0 is /a.xtc, not the new method");
  // A method was removed.
  const std::vector<std::string> removed = {"/a.xtc", "/c.xtc"};
  checkEq(method::resolveDrawnRow(drawn, 1, removed), -1, "a row that is gone resolves to nothing");
  checkEq(method::resolveDrawnRow(drawn, 2, removed), 1, "the rows after it moved up");
  checkEq(method::resolveDrawnRow(drawn, 3, drawn), -1, "past the end");
  checkEq(method::resolveDrawnRow(drawn, -1, drawn), -1, "before the start");
  checkEq(method::resolveDrawnRow({}, 0, drawn), -1, "nothing was drawn");
  checkEq(method::resolveDrawnRow(drawn, 0, {}), -1, "nothing is on the shelf now");
}

void testRing() {
  checkEq(method::ringForRow(0), 1, "first row is ring 1");
  checkEq(method::rowForRing(1), 0, "ring 1 is the first row");
  checkEq(method::rowForRing(0), -1, "ring 0 is the status line");
}

}  // namespace

int main() {
  testBox();
  testNoFinger();
  testSizes();
  testFingerNeverOnRemove();
  testFingerInTheMiddleIsOnKeep();
  testNarrowScreens();
  testResolveDrawnRow();
  testRing();
  std::cout << "method layout: " << fake::checkCount() << " checks passed\n";
  return 0;
}
