#pragma once

#include <mutex>

#include "AmbientProvider.h"
#include "MessageProtocol.h"

class MappedInputManager;

namespace ambient {
class LatestMessageProvider final : public AmbientProvider {
  MessageMetadata pending;
  uint8_t pendingSlot = 0;
  FetchResult status = FetchResult::None;
  mutable std::mutex contentMutex;
  bool cachedContentValid() const;
  bool commitPendingLocked();

 public:
  FetchResult fetchContent() override;
  FetchResult getStatus() const override;
  bool hasContent() const override;
  bool render(GfxRenderer& renderer) override;
  // Makes the frame fetchContent() just downloaded the current one without drawing it, for a timer wake that
  // must leave the panel alone. render() commits the same way after drawing; without either the next poll would
  // download the same note again.
  bool commitPending();
  static LatestMessageProvider& instance();
};

// A timer wake's join: the installed network profile, then the last-used saved network (network/NetworkJoinDevice.h).
bool connectSavedWifi();
// Radio off, after the EAP state of a profile join is cleared.
void disconnectWifi();

// A timer wake's book check (Stage B), for after the note check and before the radio goes off: the same Wi-Fi
// session and clock window. It files the new Method books the wake rules allow (src/ambient/MethodWake.h) and never
// draws. `noteFailed`: the note check ended Failed. Once a check has started it turns the radio off itself; the
// caller does on every other path. The hardware is read between the check's steps: returns true when the user pressed
// a key or touched the screen, in which case the check was cancelled through its normal teardown and the caller
// carries on as a button wake (see method::wake::ending, and settleWakePress). Implemented in
// src/activities/ambient/MethodWakeRun.cpp.
bool checkBooksOnWake(GfxRenderer& renderer, bool noteFailed);
// For a wake that a press turned into a button wake, before the boot goes on: waits (a moment) for the press to end
// and takes it into the input state, so that its release is not an action. A key or a finger still down when the wait
// runs out is treated as a consumed long-press, so that its release does not act either.
void settleWakePress(MappedInputManager& input);
}  // namespace ambient
