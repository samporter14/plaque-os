#pragma once
#include <Logging.h>

#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "ActivityManager.h"  // for using the ActivityManager singleton
#include "ActivityResult.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "RenderLock.h"
#include "util/ScreenshotInfo.h"

class Activity {
  friend class ActivityManager;

 protected:
  std::string name;
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;

  ActivityResultHandler resultHandler;
  ActivityResult result;

 public:
  explicit Activity(std::string name, GfxRenderer& renderer, MappedInputManager& mappedInput)
      : name(std::move(name)), renderer(renderer), mappedInput(mappedInput) {}
  virtual ~Activity() = default;
  virtual void onEnter();
  virtual void onExit();
  // Last chance to queue activity-owned state before sleep events are drained.
  virtual void prepareForSleep() {}
  virtual void loop() {}

  virtual void render(RenderLock&&) {}

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  virtual void requestUpdate(bool immediate = false);

  // Request an immediate render and block until it completes.
  virtual void requestUpdateAndWait();

  virtual bool skipLoopDelay() { return false; }
  virtual bool preventAutoSleep() { return false; }
  // Exclusive storage activities suspend global controls and normal activity
  // transitions so no filesystem code races a raw SD-card owner.
  virtual bool requiresExclusiveStorageLoop() const { return false; }
  // Screens that must be left on their own terms: the step timer's alert waits for them (the crash report, the SD
  // recovery and firmware picker, which may be writing the firmware). A screen under one of these counts too.
  virtual bool defersStepTimerAlert() const { return false; }
  virtual bool isReaderActivity() const { return false; }
  // The book a reader has open, known as soon as the reader exists (the remembered book in CrossPointState is empty
  // until its first page is on the panel). Empty for every activity that is not a reader.
  virtual std::string_view openBookPath() const { return {}; }
  // Returns true when the activity schedules its own forced refresh.
  virtual bool handleForcedRefresh() { return false; }
  virtual bool isHomeActivity() const { return false; }
  virtual bool handleHomeGesture() { return false; }
  virtual ScreenshotInfo getScreenshotInfo() const { return {}; }

  // Start a new activity without destroying the current one
  // Note: requestUpdate() will be invoked automatically once resultHandler finishes
  void startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler);

  // Set the result to be passed back to the previous activity when this activity finishes
  void setResult(ActivityResult&& result);

  // Finish this activity and return to the previous one on the stack (if any)
  static void finish();

  // Convenience method to facilitate API transition to ActivityManager
  // TODO: remove this in near future
  static void onGoHome(HomeMenuItem item = HomeMenuItem::NONE);
  static void onSelectBook(const std::string& path);
};
