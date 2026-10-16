#include "MethodLayout.h"

#include <algorithm>

namespace method {
namespace {
int clampInt(int value, int low, int high) {
  if (high < low) return low;
  return std::max(low, std::min(value, high));
}
}  // namespace

RemoveDialogPlacement placeRemoveDialog(const RemoveDialogSpec& spec, const bool hasFinger, const int fingerX,
                                        const int fingerY) {
  RemoveDialogPlacement out;
  const int width = std::min(spec.dialogW, spec.screenW - 2 * spec.margin);
  const int height = std::min(spec.dialogH, spec.screenH - 2 * spec.margin);
  const int choiceW = (width - spec.padLeft - spec.padRight - spec.gap) / 2;
  const int rowY = height - spec.padBottom - spec.buttonH;  // top of the choice row inside the dialog

  // Keep goes on the side of the screen the finger is on (the left choice when there is no finger).
  out.keepFirst = !hasFinger || fingerX < spec.screenW / 2;
  const int keepOffset = spec.padLeft + (out.keepFirst ? 0 : choiceW + spec.gap);

  int x = (spec.screenW - width) / 2;
  int y = (spec.screenH - height) / 2;
  if (hasFinger) {
    // Slide the dialog so the Keep choice is centred on the finger, as far as the screen allows.
    x = clampInt(fingerX - keepOffset - choiceW / 2, spec.margin, spec.screenW - spec.margin - width);
    y = clampInt(fingerY - rowY - spec.buttonH / 2, spec.margin, spec.screenH - spec.margin - height);
  }

  out.dialog = Box{x, y, width, height};
  const Box left{x + spec.padLeft, y + rowY, choiceW, spec.buttonH};
  const Box right{x + spec.padLeft + choiceW + spec.gap, y + rowY, choiceW, spec.buttonH};
  out.keep = out.keepFirst ? left : right;
  out.remove = out.keepFirst ? right : left;

  // Whatever the clamping did, the finger must never be on Remove.
  if (hasFinger && out.remove.contains(fingerX, fingerY)) {
    out.keepFirst = !out.keepFirst;
    std::swap(out.keep, out.remove);
  }
  out.keepUnderFinger = hasFinger && out.keep.contains(fingerX, fingerY);
  return out;
}

int resolveDrawnRow(const std::vector<std::string>& drawnPaths, const int drawnIndex,
                    const std::vector<std::string>& currentPaths) {
  if (drawnIndex < 0 || drawnIndex >= static_cast<int>(drawnPaths.size())) return -1;
  const std::string& path = drawnPaths[static_cast<size_t>(drawnIndex)];
  for (size_t i = 0; i < currentPaths.size(); ++i)
    if (currentPaths[i] == path) return static_cast<int>(i);
  return -1;
}

}  // namespace method
