#include <BoardConfig.h>

#include "PlaqueBoard.h"

// The device side of PlaqueBoard.h: the facts come from the SDK's active profile (BoardConfig::ACTIVE), and a
// hardware bit counts only when its driver is compiled into this build as well.
namespace plaque {

Board board() {
  BoardFacts facts;
  facts.x4Pro = BoardConfig::isX4Pro();
  facts.x4Classic = BoardConfig::isX4Classic();
#if FREEINK_CAP_TOUCH
  facts.touch = BoardConfig::hasTouch();
#endif
  facts.homeKey = facts.touch && BoardConfig::hasHomeKey();
#if FREEINK_CAP_FRONTLIGHT
  facts.frontlight = BoardConfig::hasPwmFrontlight() || BoardConfig::hasI2cFrontlight();
#endif
#if FREEINK_CAP_USB_MSC
  facts.usbDrive = true;
#endif
  return boardFor(facts);
}

}  // namespace plaque
