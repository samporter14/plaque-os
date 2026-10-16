#include <Arduino.h>
#include <BoardConfig.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalMemory.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#include <PlaqueBoard.h>
#include <SPI.h>
#include <TrustedTime.h>
#include <VectorFontSupport.h>
#include <WiFi.h>
#include <XteinkDetect.h>
#include <builtinFonts/all.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/StepTimerAlertActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "ambient/AmbientSettings.h"
#include "ambient/LatestMessageProvider.h"
#include "ambient/MethodWake.h"
#include "ambient/ReaderIdentity.h"
#include "ambient/RegisterProvider.h"
#include "ambient/SleepFrame.h"
#include "ambient/StepTimerService.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/ClockProvenance.h"
#include "network/NetProvisionProtocol.h"
#include "network/NetworkJoinDevice.h"
#include "network/NetworkProfileDevice.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/ButtonNavigator.h"
#include "util/PluginEvents.h"
#include "util/ScreenshotUtil.h"
#include "util/Timezones.h"

#if CROSSPOINT_VECTOR_FONTS
// Rendering (incl. FreeType TTF rasterization) runs on the Arduino loop task.
// The default 8 KB stack overflows inside FreeType's FT_Open_Face / variable-font
// parsing. This runtime override applies even with the prebuilt (dio_opi) core,
// where CONFIG_ARDUINO_LOOP_STACK_SIZE from sdkconfig is baked in and ignored.
// Vector-font boards only: without TTF the stock loop stack has always sufficed,
// and non-PSRAM boards need the 16KB back in DRAM.
SET_LOOP_TASK_STACK_SIZE(24 * 1024)
#endif

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts(), renderer.getTtfFonts());
static unsigned long allowSleepAt = 0;
static unsigned long lastX4ProPowerClickAt = 0;

namespace {
constexpr unsigned long X4PRO_POWER_DOUBLE_CLICK_MS = 500;
constexpr unsigned long X4PRO_POWER_CLICK_MAX_HOLD_MS = 300;
}  // namespace

// A wake hold must never become an in-app power-button action.  Boot may continue
// while the button is held; swallow the one release that ends that wake gesture.
static bool wakePowerReleasePending = false;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

EpdFont notosans12RegularFont(&notosans_12_regular);
EpdFont notosans12BoldFont(&notosans_12_bold);
EpdFont notosans12ItalicFont(&notosans_12_italic);
EpdFont notosans12BoldItalicFont(&notosans_12_bolditalic);
EpdFontFamily notosans12FontFamily(&notosans12RegularFont, &notosans12BoldFont, &notosans12ItalicFont,
                                   &notosans12BoldItalicFont);
EpdFont notosans14RegularFont(&notosans_14_regular);
EpdFont notosans14BoldFont(&notosans_14_bold);
EpdFont notosans14ItalicFont(&notosans_14_italic);
EpdFont notosans14BoldItalicFont(&notosans_14_bolditalic);
EpdFontFamily notosans14FontFamily(&notosans14RegularFont, &notosans14BoldFont, &notosans14ItalicFont,
                                   &notosans14BoldItalicFont);
EpdFont notosans16RegularFont(&notosans_16_regular);
EpdFont notosans16BoldFont(&notosans_16_bold);
EpdFont notosans16ItalicFont(&notosans_16_italic);
EpdFont notosans16BoldItalicFont(&notosans_16_bolditalic);
EpdFontFamily notosans16FontFamily(&notosans16RegularFont, &notosans16BoldFont, &notosans16ItalicFont,
                                   &notosans16BoldItalicFont);
EpdFont notosans18RegularFont(&notosans_18_regular);
EpdFont notosans18BoldFont(&notosans_18_bold);
EpdFont notosans18ItalicFont(&notosans_18_italic);
EpdFont notosans18BoldItalicFont(&notosans_18_bolditalic);
EpdFontFamily notosans18FontFamily(&notosans18RegularFont, &notosans18BoldFont, &notosans18ItalicFont,
                                   &notosans18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_TARGET_SETTINGS = 2;
constexpr uint32_t SILENT_REBOOT_TARGET_JOIN_NETWORK = 3;
constexpr uint32_t SILENT_REBOOT_TARGET_MAX = SILENT_REBOOT_TARGET_JOIN_NETWORK;
constexpr uint32_t SILENT_REBOOT_LIGHT_ON = 1U << 0;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,          // cold boot, flash, panic, or plain reboot
  Silent,          // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SplashlessWake,  // wake from deep sleep with the splash suppressed by the SD flag
};

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

// Set by an activity that wants the reader asleep as an idle timeout would put it there (the step timer's alert, after
// it has run its course on a reader that woke by itself); acted on at the top of loop().
static bool deepSleepRequested = false;
void requestDeepSleepSoon() { deepSleepRequested = true; }

// A silent restart is internal maintenance, so the light must come back exactly
// as the user left it. SETTINGS.frontlightOn is the saved preference and
// legitimately diverges from the live state (a wake with Restore Light on Wake
// off leaves the light off while the saved "was on" preference is kept), so
// carry the live state across the reboot instead of re-deriving it from
// settings. Cleared with the magic in setup().
static void armSilentReboot(const uint32_t target) {
  silentRebootTarget = target;
  silentRebootPayload = Frontlight.isOn() ? SILENT_REBOOT_LIGHT_ON : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
}

// Returns instead of rebooting when sleep supersedes the reboot; callers keep
// running in that case.
static void silentRestartTo(const uint32_t target, const char* targetName) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  armSilentReboot(target);
  LOG_DBG("MAIN", "Silent restart (target=%s)", targetName);
  // E-ink retains the previous frame until the target's first paint lands
  // (~2-3s). Without an overlay, users don't see the reboot and fire input
  // through to the new activity. On Home, Select on the default
  // selectorIndex=0 opens the most-recent book, looking like a trampoline back
  // to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  STEP_TIMER.persist();  // a running step timer is held on the counter this restart resets: write its end down first
  ESP.restart();
}

void silentRestart() { silentRestartTo(SILENT_REBOOT_TARGET_HOME, "home"); }

void silentRestartToReader() { silentRestartTo(SILENT_REBOOT_TARGET_READER, "reader"); }

void silentRestartToSettings() { silentRestartTo(SILENT_REBOOT_TARGET_SETTINGS, "settings"); }

void silentRestartToJoinNetwork() {
  if (deepSleepInProgress) return;
#if FREEINK_CAP_TOUCH
  // A software reset would cycle touch/frontlight rails; those boards proceed
  // into Join Network without the fresh-heap reboot (return, don't stop WiFi —
  // this runs on the way *in*, unlike the exit-time silentRestart()).
  if (BoardConfig::hasTouch()) return;
#endif
  armSilentReboot(SILENT_REBOOT_TARGET_JOIN_NETWORK);
  LOG_DBG("MAIN", "Silent restart (target=join-network)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  STEP_TIMER.persist();  // a running step timer is held on the counter this restart resets: write its end down first
  ESP.restart();
}

void restartToHomeAfterStorageHandoff() {
  if (deepSleepInProgress) return;  // sleeping supersedes the storage handoff reboot
  armSilentReboot(SILENT_REBOOT_TARGET_HOME);
  LOG_DBG("MAIN", "Restart after storage handoff (target=home)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  handoffUsbOtgToSerialJtag();
  STEP_TIMER.persist();
  ESP.restart();
}

void toggleFrontlight() {
  if (!Frontlight.present()) return;
  const bool lightOn = !Frontlight.isOn();
  Frontlight.setOn(lightOn);
  SETTINGS.frontlightOn = lightOn ? 1 : 0;
  SETTINGS.saveToFile();
  LOG_INF("LIGHT", "Frontlight toggled %s", lightOn ? "on" : "off");
}

bool handleX4ProFrontlightDoubleClick() {
  if (!BoardConfig::isX4Pro() || !SETTINGS.doubleClickPwrLight || !gpio.wasReleased(HalGPIO::BTN_POWER)) {
    return false;
  }

  const unsigned long now = millis();
  if (gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS) {
    lastX4ProPowerClickAt = 0;
    return false;
  }

  if (lastX4ProPowerClickAt == 0 || now - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = now;
    return false;
  }

  lastX4ProPowerClickAt = 0;
  toggleFrontlight();
  return true;
}

constexpr char SLEEP_FRAME_FILE[] = "/.crosspoint/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

// Plugin-event delivery on the way into deep sleep. sleep.enter is delivered
// now — over the live connection, or by bringing WiFi up when a plugin
// subscribes (e.g. fetching a fresh /sleep.bmp so THIS sleep shows it — the
// drain runs before goToSleep() renders the sleep screen). The connect path
// is bounded (join deadline + drain event budget), skipped on low battery,
// and sleep is never blocked on the network: a failed join or delivery just
// sleeps with the previous image and the queued events retry on the next
// drain (at-least-once). The caller's WiFi shutdown tears the radio down
// either way. Deferrable events already queued (reader.exit) ride along in
// the same drain.
//
// keepPanel: a Method step is about to be left on the panel. Nothing may be
// drawn over it, so the join's "Loading" popup and any handler toast (drain()
// draws them with the renderer) are skipped; delivery itself is unchanged.
static void deliverSleepPluginEvents(const bool keepPanel) {
  // Activity-owned state must be queued before sleep.enter and before this
  // same-sleep drain. The hook is idempotent with ordinary activity teardown.
  activityManager.prepareForSleep();

  // Sleeping straight out of a book is the common flow, but the reader's own
  // reader.exit only fires later, inside goToSleep() — after this drain. Carry
  // the book and progress on sleep.enter itself so a sync handler bound to it
  // pushes current progress on THIS connection, not the next one.
  pluginevents::Var vars[2];
  size_t varCount = 0;
  char percent[8];
  const ScreenshotInfo info = activityManager.getScreenshotInfo();
  if (info.readerType != ScreenshotInfo::ReaderType::None && !APP_STATE.openEpubPath.empty()) {
    snprintf(percent, sizeof(percent), "%d", info.progressPercent);
    vars[varCount++] = {"book", APP_STATE.openEpubPath.c_str()};
    vars[varCount++] = {"percent", percent};
  }
  pluginevents::emit(pluginevents::Event::SleepEnter, vars, varCount);
  GfxRenderer* const toastRenderer = keepPanel ? nullptr : &renderer;
  if (WiFi.status() == WL_CONNECTED) {
    pluginevents::drain(toastRenderer);
    return;
  }
  // Any connect-flagged queued event justifies the join, not only
  // sleep.enter: reader.session is queued while reading and delivered on this
  // same sleep, and a progress-sync plugin usually subscribes to it alone.
  if (!pluginevents::wantsConnectAny()) return;
  if (powerManager.getBatteryPercentage() < 20) return;
  // The same join a timer wake makes: the installed network profile, then the last-used saved network. Nothing to
  // join with is the end of it (and no "Loading" popup for nothing).
  if (!netjoin::hasWakeCandidate()) return;

  if (!keepPanel) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  if (ambient::connectSavedWifi()) {
    trustedtime::startSync();  // snap the clock floor while the network is up
    pluginevents::drain(toastRenderer);
  } else {
    LOG_DBG("MAIN", "Sleep-event WiFi join timed out; deferring delivery");
  }
}

// Set when the card would not mount at a step timer's end and the alert was shown without it (see setup()). Nothing may
// then write the card's state files: they were never read, so what would be written is the defaults.
static bool cardlessBoot = false;

// Sleep on a boot with no card: the panel keeps the alert, nothing is saved, and no timer is left to wake for.
static void cardlessDeepSleep() {
  HalPowerManager::Lock powerLock;
  deepSleepInProgress = true;
  // The alert's onExit, which puts the light back, does not run on this path: a sleep asked for mid-pulse (the power
  // button) would otherwise leave the light on.
  Frontlight.setOn(false);
  halTiltSensor.deepSleep();
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep (no card)");
  powerManager.startDeepSleep(gpio, STEP_TIMER.rearm(0));
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout = false) {
  if (cardlessBoot) {
    cardlessDeepSleep();
    return;
  }
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();

  // Sleep may end in a power-off (battery death, latch); persist the clock
  // floor now so a later cold boot resumes from it.
  trustedtime::note();

  // A Method step outranks the sleep-screen setting and the ambient screen: sleeping inside a Method book is
  // always a Quick Resume sleep (see ambient/SleepFrame.h). Asked first, before anything can draw, so the
  // sleep-time plugin popups stay off the step, and so that sleep skips the ambient frame check (a 48 KB read
  // and hash).
  //
  // A reader that opened a book less than a second ago has not remembered it yet (it clears the path on entry and sets
  // it after the first page), so the open reader's own path stands in for an empty one.
  const plaque::Capabilities caps = plaque::board().caps;
  const std::string readerBook = activityManager.readerBookPath();
  const sleepframe::SleepBook sleepBook =
      sleepframe::resolveSleepBook(caps.method, APP_STATE.lastSleepFromReader, APP_STATE.openEpubPath, readerBook);
  const bool methodStep = sleepframe::methodStepOpen(caps.method, APP_STATE.lastSleepFromReader, sleepBook.path);

  // For the step timer's alert after this sleep: which page of its book the panel will keep (0 when it is not a page of
  // that book). Asked before the sleep screen replaces the reader.
  int timerPanelPage = 0;
  if (methodStep && STEP_TIMER.running()) {
    const ScreenshotInfo info = activityManager.getScreenshotInfo();
    if (info.readerType == ScreenshotInfo::ReaderType::Xtc && readerBook == STEP_TIMER.record().bookPath)
      timerPanelPage = info.currentPage;
  }

  deliverSleepPluginEvents(methodStep);

  // The Register screen (an instrument's schedule) is the sleep screen when it is chosen and a schedule is on the card;
  // it is a per-reader choice and wins over the note. The panel holds nothing of it unless the draw below puts it
  // there. Both screens are mailbox frames, so both need a board that offers the sleep screen
  // (plaque::ambientSleepScreen).
  auto& registerScreen = ambient::RegisterProvider::instance();
  registerScreen.clearPanel();
  if (!AMBIENT_SETTINGS.registerSelected()) ambient::RegisterProvider::disarm();
  const bool registerSleep =
      !methodStep && AMBIENT_SETTINGS.registerSelected() && registerScreen.prepare(powerManager.getBatteryPercentage());
  const bool ambientSleep =
      !methodStep && plaque::ambientSleepScreen(caps) &&
      (registerSleep || (AMBIENT_SETTINGS.useAsSleepScreen && ambient::LatestMessageProvider::instance().hasContent()));
  const bool quickResumeSetting =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
      (fromTimeout &&
       SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
  const sleepframe::SleepPlan sleepPlan = sleepframe::planSleep(
      {caps.method, APP_STATE.lastSleepFromReader, sleepBook.path, quickResumeSetting, ambientSleep});
  const bool isQuickResumeSleep = sleepPlan.frame == sleepframe::Frame::QuickResume;
  // The wake reopens the remembered book (sleepframe::resumesBook), so a Method book that was still opening is
  // remembered now. The plugin events above have already been delivered with the state as it was.
  if (sleepBook.remember) {
    APP_STATE.openEpubPath = sleepBook.path;
    LOG_INF("SLP", "Method book still opening: remembered for the wake (%s)", sleepBook.path.c_str());
  }
  // Persisted with the state save below, and read by SleepActivity (which draws the moon over the page) and by
  // the timer-wake path (which must not draw over it). Assigned on every sleep, so it cannot go stale.
  APP_STATE.methodStepOnPanel = sleepPlan.methodStep;
  if (sleepPlan.methodStep)
    LOG_INF("SLP", "Method step open: quick resume sleep, marker set (%s)", APP_STATE.openEpubPath.c_str());
  // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
  // it visible until the first useful reader or home paint replaces it.
  APP_STATE.showBootScreen = false;

  APP_STATE.saveToFile();

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  activityManager.goToSleep(fromTimeout);

  if (isQuickResumeSleep) {
    saveSleepFrameBuffer();
  } else if (Storage.exists(SLEEP_FRAME_FILE)) {
    // A stale Quick Resume frame must not replace the selected sleep screen during wake.
    Storage.remove(SLEEP_FRAME_FILE);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive. disconnectWifi() also clears the
  // supplicant's EAP state if the sleep-event delivery joined through a certificate profile.
  if (WiFi.getMode() != WIFI_MODE_NULL) ambient::disconnectWifi();

  halTiltSensor.deepSleep();
  display.deepSleep();
  Storage.prepareForDeepSleep();
  LOG_DBG("MAIN", "Entering deep sleep");

  // The ambient poll, or the step timer's end if that comes first. Read last, after the sleep screen: that takes
  // seconds, and the timer counts them. The Register screen also wakes for the next booking to start or end; its plan
  // already holds the step timer's end, and rearm() records the page on the panel.
  const uint64_t ambientWake =
      AMBIENT_SETTINGS.registerSelected()
          ? registerScreen.wakeMicros(powerManager.getBatteryPercentage(), sleepPlan.methodStep)
          : AMBIENT_SETTINGS.wakeTimerMicros();
  powerManager.startDeepSleep(gpio, STEP_TIMER.rearm(ambientWake, timerPanelPage));
}

// The step timer ended while the reader is awake. In a book (a reader on screen or under the screen that is) the alert
// goes over it and a press goes back; anywhere else it replaces that screen and a press opens the timer's book. Either
// way the timer is marked alerted as the alert starts, so this runs once per timer.
static void startStepTimerAlertIfDue() {
  // USB Drive owns the card (nothing may navigate); the crash report and the SD recovery and firmware picker must not
  // be replaced either. The timer waits, and the alert comes up when that screen is left.
  const bool screenBusy = activityManager.requiresExclusiveStorageLoop() || activityManager.defersStepTimerAlert();
  if (deepSleepInProgress || !steptimer::alertWanted(STEP_TIMER.running(), STEP_TIMER.remainingMs(),
                                                     StepTimerAlertActivity::gate().pending(), screenBusy))
    return;
  const steptimer::Record rec = STEP_TIMER.record();
  const bool inReader = activityManager.isReaderActivity();
  StepTimerAlertActivity::Params alert;
  alert.record = rec;
  // A boot whose card would not mount and that a person woke (the "SD card error" screen) reaches here too, with the
  // timer ending while the error is on the panel. The alert then ends on that screen again, as the one made at the end
  // of a sleep does, not on a Home built from state this boot never read.
  alert.after = !Storage.ready() ? StepTimerAlertActivity::After::NoCard
                : inReader       ? StepTimerAlertActivity::After::Pop
                                 : StepTimerAlertActivity::After::OpenBook;
  if (inReader) {
    const ScreenshotInfo info = activityManager.getScreenshotInfo();
    if (info.readerType == ScreenshotInfo::ReaderType::Xtc && activityManager.readerBookPath() == rec.bookPath)
      alert.panelPage = static_cast<unsigned>(info.currentPage);
  }
  LOG_INF("TIMER", "The timer ended while awake (%s)", inReader ? "over the reader" : "replacing the screen");
  // Not marked alerted here: the alert marks it as it comes up (see StepTimerAlertActivity). A screen that restarts the
  // reader as it is left then leaves a running, over timer behind, which the next boot alerts for.
  auto activity = std::make_unique<StepTimerAlertActivity>(renderer, mappedInputManager, alert);
  if (inReader) {
    activityManager.pushActivity(std::move(activity));
  } else {
    activityManager.replaceActivity(std::move(activity));
  }
}

void setupDisplayAndFonts(bool seamless = false) {
#if !FREEINK_MCU_C3
  // C3 resolves its controller in HalGPIO::begin() before SPI claims the
  // display pins. X4 Pro skips that C3-only path, so probe here before
  // display.begin() selects and initializes its panel driver.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    if (freeink::applyXteinkDisplayController()) {
      LOG_DBG("MAIN", "Panel controller: UltraChip UC81xx variant detected");
    }
  }
#endif

  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

  renderer.insertFont(NOTOSANS_12_FONT_ID, notosans12FontFamily);
  renderer.insertFont(NOTOSANS_14_FONT_ID, notosans14FontFamily);
  renderer.insertFont(NOTOSANS_16_FONT_ID, notosans16FontFamily);
  renderer.insertFont(NOTOSANS_18_FONT_ID, notosans18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  // Discover and load SD card fonts
  sdFontSystem.begin(renderer);

  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  BoardConfig::holdPowerRails();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  Serial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
  // The clocks, read before anything can step the wall clock: a step timer's end is put on this boot's counter from
  // them.
  const steptimer::Now bootClocks = steptimer::readClocks();
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_MAX) ? silentRebootTarget : 0;
  const bool silentRebootLightOn = isSilentReboot && (silentRebootPayload & SILENT_REBOOT_LIGHT_ON) != 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;

  gpio.begin();
  powerManager.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  // Sample the wake hold now — a click wake is released within milliseconds of
  // boot — but defer the sleep-or-boot decision until SETTINGS is loaded below:
  // click-to-wake is a setting, and an X4 battery power-off cuts all power, so
  // only SD state survives to the next boot.
  const bool wakeHoldVerified = wakeupReason != HalGPIO::WakeupReason::PowerButton || gpio.verifyPowerButtonWakeup();

  // X4 Pro and X4 Classic both map BTN_UP to GPIO0 — an ESP32-S3 boot strap — so
  // gate recovery on the non-strap Down key (GPIO7) to avoid a stuck-in-recovery loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? MappedInputManager::Button::Down
                                                                                     : MappedInputManager::Button::Up;
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    mappedInputManager.isPressed(recoveryButton);

  halTiltSensor.begin();
  halClock.begin();

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif

  // What Plaque offers on this board (lib/PlaqueBoard): the profile is final once gpio.begin() has run.
  const plaque::Capabilities caps = plaque::board().caps;

  // An ambient poll's wake (a board with timed wake): it has its own path below, before any display work.
  const bool ambientTimerWake = caps.timedWake && esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;

  // A step timer on the reader: put its end on this boot's counter and decide what this boot has to do about it. This
  // comes before the ambient poll below, because a timer that has ended is the reason for this wake and the poll waits
  // for the next one. The wake hook lets every re-sleep that draws nothing (a poll, a power bounce) also wake for the
  // timer's end.
  steptimer::BootAction stepAction = steptimer::BootAction::None;
  if (caps.stepTimers) {
    stepAction = STEP_TIMER.boot(bootClocks, ambientTimerWake);
    sleepframe::setWakeHook([](uint64_t pollMicros) -> uint64_t { return STEP_TIMER.rearm(pollMicros); });
  }
  const bool stepAlertWake =
      stepAction == steptimer::BootAction::AlertPulse || stepAction == steptimer::BootAction::AlertQuiet;
  // A poll that would still be running when the timer ends would make its alert late (the Wi-Fi join, the note and the
  // book check can take 150 s, 160 to 175 s with a network profile): skip it, sleep again for the timer's end, and poll
  // at the next sleep.
  if (ambientTimerWake && !stepAlertWake &&
      !steptimer::pollFitsBeforeEnd(STEP_TIMER.running(), STEP_TIMER.remainingMs(),
                                    steptimer::pollBudgetMs(netjoin::wakeJoinWorstMsNow()))) {
    LOG_INF("TIMER", "Poll skipped: the timer ends in %lld ms", static_cast<long long>(STEP_TIMER.remainingMs()));
    halTiltSensor.deepSleep();
    Storage.prepareForDeepSleep();
    powerManager.startDeepSleep(gpio, STEP_TIMER.rearm(0));
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  bool cardMounted = Storage.begin();
  // A card that will not mount at a step timer's end must not cost the alert: try again, twice. The same for a timer
  // wake the Register screen armed: its schedule is on the card, and a wake that cannot read it must not end the chain.
  const bool registerWakeArmed = ambientTimerWake && ambient::RegisterProvider::armed();
  for (int retry = 0; !cardMounted && retry < 2 && (STEP_TIMER.running() || stepAlertWake || registerWakeArmed);
       ++retry) {
    LOG_ERR("MAIN", "SD card initialization failed; trying again");
    delay(250);
    cardMounted = Storage.begin();
  }
  if (!cardMounted) {
    LOG_ERR("MAIN", "SD card initialization failed");
    const steptimer::NoCardPlan plan =
        steptimer::noCardPlan(stepAction, ambientTimerWake, STEP_TIMER.running(), STEP_TIMER.remainingMs());
    switch (plan) {
      case steptimer::NoCardPlan::ShowAlert: {
        // The alert needs nothing from the card: the defaults stand in for the settings, the built-in fonts draw it on
        // a clean screen, and the record it is made from is in NVS. A press then goes on to the card error.
        cardlessBoot = true;
        Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, false);
        setupDisplayAndFonts(true);
        StepTimerAlertActivity::Params alert;
        alert.record = STEP_TIMER.record();
        alert.after = StepTimerAlertActivity::After::NoCard;
        alert.retainedFrame = false;
        alert.quiet = stepAction == steptimer::BootAction::AlertQuiet;
        alert.lostPower = STEP_TIMER.lostPower();
        alert.fromTimerWake = ambientTimerWake;
        alert.waitMs = std::max<int64_t>(0, STEP_TIMER.remainingMs());
        activityManager.replaceActivity(std::make_unique<StepTimerAlertActivity>(renderer, mappedInputManager, alert));
        return;
      }
      case steptimer::NoCardPlan::SleepForTimer:
      case steptimer::NoCardPlan::SleepQuietly:
        // The retained frame may be a Method step: leave the panel alone and sleep again. The ambient settings are
        // unreadable, so no poll; a step timer still ahead is woken for (it lives in NVS, not on the card). The next
        // button wake shows the error.
        halTiltSensor.deepSleep();
        Storage.prepareForDeepSleep();
        // A Register wake that cannot read the card tries again soon (the step timer's end, if it comes first, is
        // already in that wake); without Register the wake chain ends here, as it always did.
        powerManager.startDeepSleep(
            gpio, registerWakeArmed ? ambient::RegisterProvider::cardRetryMicros(powerManager.getBatteryPercentage())
                                    : (plan == steptimer::NoCardPlan::SleepForTimer ? STEP_TIMER.rearm(0) : 0));
        break;
      case steptimer::NoCardPlan::ShowError:
        break;
    }
    setupDisplayAndFonts(isSilentReboot);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  APP_STATE.loadFromFile();
  // Not const: a press during a timer wake's book check turns that wake into a button wake (below). A step timer's
  // alert is a wake like a button's as far as the panel goes: it resumes into the retained frame, no splash.
  bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton || stepAlertWake;
  bool isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || BoardConfig::isX4Classic()) ? "DOWN" : "UP");
  }

  // Touch boards default the reader menu to the toolbar overlay instead of the
  // full-screen list. Seeded before the load: fromJson() falls back to the
  // in-memory value only when the file carries no readerMenuStyle key, so a
  // user's saved choice (either style) still wins.
  if (gpio.hasTouch()) {
    SETTINGS.readerMenuStyle = CrossPointSettings::READER_MENU_TOOLBAR;
  }
  SETTINGS.loadFromFile();
  bool ambientSettingsRead = true;
  if (caps.mailbox) {
    ambientSettingsRead = AMBIENT_SETTINGS.loadFromFile();
    MESSAGE_STATE.loadFromFile();
  }
  // The reader's own identity, made on the first boot, and /Lamina/reader.json (model, Wi-Fi addresses, capabilities)
  // rewritten when it differs: every boot, before any network, so the Mac can read the station MAC off a reader that
  // has never been online. A read and a compare when the file is right.
  if (caps.cardIdentity) ambient::syncReaderCard();
  // Push the saved timezone's POSIX rule into the clock (migrating the legacy
  // UTC-offset setting on first boot after the update).
  timezones::applyToClock();
  RECENT_BOOKS.loadFromFile();
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  KOREADER_STORE.loadFromFile();
  OPDS_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  pluginevents::refreshSubscriptions();
  // Restore the monotonic clock floor before anything reads time() (event
  // timestamps, loan-expiry checks).
  trustedtime::init();
  // Whether the clock kept counting since the last recorded sync (a deep-sleep wake, or the RTC chip): the floor
  // restored above never counts as current time.
  clockprov::begin();
  // Every caller of a sync gets the installed profile's time servers (startSync(), beginSync(), syncNow()), and every
  // sync that sets the RTC chip goes through the clock record's judgement; none of them has to remember to.
  trustedtime::setSyncPrepareHook(&netprofile::applyTimeSettings);
  halClock.setSyncRecorder([]() { return clockprov::recordSync() == clockprov::Verdict::Record; });

  // A timer wake runs before display initialization or reader routing. No-change
  // and failed polls leave the retained panel image and reading state untouched.
  if (ambientTimerWake && !stepAlertWake) {
    if (!ambientSettingsRead && ambient::RegisterProvider::armed()) {
      // The mailbox settings did not load (a failed read of the card, once): with Register armed that must not end the
      // wake chain, or the panel would say "In use" for ever. Sleep again and try again soon.
      LOG_ERR("AMBIENT", "Timer wake: the mailbox settings could not be read");
      ambient::disconnectWifi();
      halTiltSensor.deepSleep();
      const uint64_t retry = ambient::RegisterProvider::cardRetryMicros(powerManager.getBatteryPercentage());
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio, retry);
    }
    Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, false);
    LOG_INF("AMBIENT", "Timer wake (Method step on the panel: %s)", APP_STATE.methodStepOnPanel ? "yes" : "no");
    bool buttonWake = false;  // a press during the book check made this a button wake: carry on into the boot
    // The Register screen (an instrument's schedule) wakes for two reasons that both arrive as this timer wake: the
    // poll asks the mailbox, a boundary (a booking starts or ends) only redraws, with no network at all. Which one it
    // is comes from the clock (reg::pollDue). With Register chosen the note is not polled: the mailbox has none for a
    // registered reader.
    auto& registerScreen = ambient::RegisterProvider::instance();
    const bool registerMode = AMBIENT_SETTINGS.registerSelected();
    const uint16_t battery = powerManager.getBatteryPercentage();
    const bool polling =
        registerMode ? registerScreen.pollWake(battery) : AMBIENT_SETTINGS.wakeTimerMicros() && battery >= 15;
    // What the panel should say now, drawn only if it would change (and never over a Method step).
    const auto redrawRegister = [&] {
      if (reg::shouldRedraw(registerScreen.planRedraw(battery, APP_STATE.methodStepOnPanel))) {
        setupDisplayAndFonts(true);
        registerScreen.drawPrepared(renderer);
        display.deepSleep();
      }
    };
    if (polling && ambient::connectSavedWifi()) {
      const auto result =
          registerMode ? registerScreen.fetch() : ambient::LatestMessageProvider::instance().fetchContent();
      if (registerMode) registerScreen.notePolled();
      // Methods arrive while asleep: the book check shares this Wi-Fi session and the clock the note check just
      // synced, files the new books the wake rules allow, and never draws, with a Method step on the panel or not.
      // What is drawn below depends on the note (or the schedule) alone. It reads the keys and the touch screen
      // between its steps: a press cancels it (partial file removed, radio off) and the wake becomes a button wake.
      const bool pressed = ambient::checkBooksOnWake(renderer, result == ambient::FetchResult::Failed);
      ambient::disconnectWifi();
      const method::wake::Ending ending =
          method::wake::ending(pressed, !registerMode && result == ambient::FetchResult::Updated);
      if (ending.continueBoot) {
        // The user is here: no redraw and no sleep. The boot below does what a button wake does (the splash is
        // skipped, the book on the panel is reopened, the Method marker is cleared), after the press has ended.
        LOG_INF("AMBIENT", "Press during the wake: carrying on as a button wake");
        buttonWake = true;
        isSleepWake = ending.sleepWake;
        isPersistedSleepWake = isSleepWake && !APP_STATE.showBootScreen;
        if (ending.commitNote) {
          const bool saved = ambient::LatestMessageProvider::instance().commitPending();
          LOG_INF("AMBIENT", "New note %s, not drawn", saved ? "saved" : "NOT saved");
        }
        wakePowerReleasePending = ending.swallowRelease;
        // A finished step timer is left for whoever wakes the reader, and this press is that person: acknowledge it
        // as any boot by someone using the reader does (the step timer's own rule, with timerWake false).
        const int64_t timerLeftMs = STEP_TIMER.running() ? STEP_TIMER.remainingMs() : 0;
        if (steptimer::planBoot({STEP_TIMER.record().phase, false, timerLeftMs, false}) ==
            steptimer::BootAction::Acknowledge)
          STEP_TIMER.clear();
        ambient::settleWakePress(mappedInputManager);
      } else if (registerMode) {
        // The schedule is on the card already. While a Method step is on the panel nothing is drawn, as for the note.
        redrawRegister();
      } else {
        // A new note is always fetched. While a Method step is on the panel it is saved but not drawn: the step
        // outranks the weather. The marker is not touched here (planWake keeps it), so this re-sleep, and every
        // later timer wake, protect the step too, and the quick-resume state a button wake needs stays as it was.
        const sleepframe::WakePlan wake = sleepframe::planWake(sleepframe::Wake::Timer, APP_STATE.methodStepOnPanel,
                                                               result == ambient::FetchResult::Updated);
        if (wake.redraw) {
          setupDisplayAndFonts(true);
          ambient::LatestMessageProvider::instance().render(renderer);
          display.deepSleep();
        } else if (wake.commitFetched) {
          const bool saved = ambient::LatestMessageProvider::instance().commitPending();
          LOG_INF("AMBIENT", "Method step on the panel: new note %s, panel left alone", saved ? "saved" : "NOT saved");
        }
      }
    } else if (registerMode) {
      // A boundary wake, or a poll with no Wi-Fi to make it: no network, the screen only moves on with the clock (and
      // says so when the schedule has aged).
      if (polling) registerScreen.notePolled();
      redrawRegister();
    }
    if (!buttonWake) {
      ambient::disconnectWifi();
      halTiltSensor.deepSleep();
      // Asked before the card is shut down: the Register screen's next boundary comes from its schedule.
      const uint64_t wake =
          registerMode
              ? registerScreen.wakeMicros(powerManager.getBatteryPercentage(), APP_STATE.methodStepOnPanel)
              : sleepframe::rearmMicros(AMBIENT_SETTINGS.wakeTimerMicros(), powerManager.getBatteryPercentage());
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio, wake);
    }
  }

  // The installed network profile's description on the card, for the Mac (no secrets; written only when it differs).
  netprofile::refreshSummaryFile();

  // Brightness and warmth are always restored. A normal wake starts with the
  // light off unless Restore Light on Wake is enabled; silent maintenance
  // reboots replay the live state captured at restart, so they neither go dark
  // nor light up against the user's wake preference.
  const bool restoreLightOn =
      isSilentReboot ? silentRebootLightOn : (SETTINGS.frontlightOn != 0 && SETTINGS.frontlightRestoreOnWake != 0);
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      // With Short Power Button Press = Sleep, a single click wakes on any
      // device; otherwise the button must still be held (ghost-wake debounce).
      if (!wakeHoldVerified && SETTINGS.shortPwrBtn != CrossPointSettings::SHORT_PWRBTN::SLEEP) {
        LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
        // Re-armed like the timer re-sleep above, so a brief power bounce does not end scheduled polling. The
        // panel is untouched, so a Method step (and its marker) stays as the sleep left it. Asked before the card is
        // shut down: the Register screen's next boundary comes from its schedule, which is read first.
        if (AMBIENT_SETTINGS.registerSelected()) ambient::RegisterProvider::instance().loadSchedule();
        const uint64_t wake =
            AMBIENT_SETTINGS.registerSelected()
                ? ambient::RegisterProvider::instance().wakeMicros(powerManager.getBatteryPercentage(),
                                                                   APP_STATE.methodStepOnPanel)
                : sleepframe::rearmMicros(AMBIENT_SETTINGS.wakeTimerMicros(), powerManager.getBatteryPercentage());
        Storage.prepareForDeepSleep();
        powerManager.startDeepSleep(gpio, wake);
      }
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // Most devices return to sleep after a USB-powered cold boot.
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
#if FREEINK_DEVICE_X4PRO || FREEINK_DEVICE_X4CLASSIC || FREEINK_DEVICE_PAPERMONO || FREEINK_DEVICE_EEGO_A4
      // X4 Pro must stay awake so USB Serial/JTAG remains available after leaving
      // USB Drive and reconnecting the cable. Paper Mono has no armable GPIO wake
      // (its button is behind the PMIC). EEGO A4's post-flash reset reads as
      // POWERON (native-USB), so a flash would otherwise be misclassified as a
      // USB-power cold boot and sleep. Sleeping any of these here would strand
      // the device in a USB-replug boot loop (or sleep right after a flash).
      break;
#else
      Storage.prepareForDeepSleep();
      powerManager.startDeepSleep(gpio);
      break;
#endif
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // This boot repaints the panel, so no Method step is held on it any more. A timer wake never gets here (it
  // re-sleeps above), and neither does a ghost wake that goes straight back to sleep, so the marker survives both.
  const sleepframe::Wake wakeKind = isSleepWake ? sleepframe::Wake::Button : sleepframe::Wake::Other;
  const bool methodStepAfterWake = sleepframe::planWake(wakeKind, APP_STATE.methodStepOnPanel, false).markerAfter;
  if (methodStepAfterWake != APP_STATE.methodStepOnPanel) {
    APP_STATE.methodStepOnPanel = methodStepAfterWake;
    APP_STATE.saveToFile();
    LOG_INF("MAIN", "Wake: Method step marker cleared");
  }
  // Someone is using the reader after a step timer's end was shown: that is the acknowledgement. (A ghost wake or an
  // ambient poll never gets here, so it leaves the finished timer for the person.)
  if (stepAction == steptimer::BootAction::Acknowledge) STEP_TIMER.clear();

  LOG_DBG("MAIN", "Starting Plaque version " CROSSPOINT_VERSION);

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  // Only a verified deep-sleep wake may use the one-shot persisted flag.
  // Otherwise a stale flag could suppress the splash on a cold boot.
  const BootResume resume = isSilentReboot         ? BootResume::Silent
                            : isPersistedSleepWake ? BootResume::SplashlessWake
                                                   : BootResume::Splash;
  bool allowFastInitialReaderRefresh = false;
  bool needsWakeRefresh = false;
  bool retainedFrameLoaded = false;  // the Quick Resume frame the sleep saved is back in the framebuffer

  setupDisplayAndFonts(resume != BootResume::Splash);

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::SplashlessWake:
      // One-shot flag: re-arm the splash for the next ordinary boot. Save
      // before any painting so a hang in the blocking paint path can't strand
      // us in a splashless-with-no-frame loop on the next boot.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (Storage.exists(SLEEP_FRAME_FILE) && loadSleepFrameBuffer()) {
        retainedFrameLoaded = true;
        if (gpio.deviceIsX3()) {
          // begin() clears the X3 controller RAM, so restore the saved frame as
          // the baseline for the first reader paint without refreshing the panel.
          renderer.cleanupGrayscaleWithFrameBuffer();
          allowFastInitialReaderRefresh = true;
        }
      } else {
        // Clean the retained sleep image as part of the first Home paint.
        needsWakeRefresh = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  // Output polarity is resolved per render by ActivityManager (night mode
  // inverts only the reading surfaces), so nothing to restore here.

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_JOIN_NETWORK) {
    // Rebooted on the way *into* File Transfer > Join Network for a fresh heap;
    // resume that flow directly instead of landing on home.
    activityManager.goToJoinNetwork();
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_SETTINGS) {
    // Back out of the WiFi rows and the user is where they left off, not on Home.
    activityManager.goToSettings();
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
  } else if (stepAlertWake) {
    // A step timer ended while the reader slept (or is found over): the alert comes first, over the frame the sleep
    // left on the panel when there is one, and a press then goes on into the book as any wake does.
    StepTimerAlertActivity::Params alert;
    alert.record = STEP_TIMER.record();
    alert.after = StepTimerAlertActivity::After::ResumeWake;
    alert.retainedFrame = retainedFrameLoaded;
    alert.quiet = stepAction == steptimer::BootAction::AlertQuiet;
    alert.lostPower = STEP_TIMER.lostPower();
    alert.fromTimerWake = ambientTimerWake;
    alert.panelPage = STEP_TIMER.record().panelPage;
    alert.waitMs = std::max<int64_t>(0, STEP_TIMER.remainingMs());
    activityManager.replaceActivity(std::make_unique<StepTimerAlertActivity>(renderer, mappedInputManager, alert));
  } else if (!sleepframe::resumesBook(APP_STATE.openEpubPath, APP_STATE.lastSleepFromReader,
                                      mappedInputManager.isPressed(MappedInputManager::Button::Back),
                                      APP_STATE.readerActivityLoadCount)) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome(HomeMenuItem::NONE, needsWakeRefresh);
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    LOG_INF("MAIN", "Wake: resuming %s", path.c_str());
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
  }

  if (resume == BootResume::Silent) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  allowSleepAt = millis() + 2000;
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.setSharedConfirmPowerShortPressEmitsPower(SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP);
  mappedInputManager.update();

  if (activityManager.requiresExclusiveStorageLoop()) {
    // USB Drive handed the raw SD card to the host. Do not run screenshots,
    // sleep, shortcuts, or normal navigation while its filesystem is detached.
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      delay(50);
    }
    return;
  }

  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, activityManager.isReaderActivity());

  renderer.setFadingFix(SETTINGS.fadingFix);

  // The ROM console does not depend on Arduino USB CDC's connection state.
  if ((Serial || FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF) && millis() - lastMemPrint >= 10000) {
    const auto heap = HalMemory::getInternalHeap();
    LOG_INF("MEM", "Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes", heap.freeBytes,
            heap.totalBytes, heap.minFreeBytes, heap.largestBlockBytes);
#ifdef BOARD_HAS_PSRAM
    const auto psram = HalMemory::getPsramHeap();
    LOG_INF("MEM", "PSRAM: Free: %zu bytes, Total: %zu bytes, Min Free: %zu bytes, MaxAlloc: %zu bytes",
            psram.freeBytes, psram.totalBytes, psram.minFreeBytes, psram.largestBlockBytes);
#endif
    lastMemPrint = millis();
  }

  // Handle incoming serial commands, unless the USB setup link (Connect to Mac) owns the input.
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (!netprov::serialInputClaimed() && logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        const uint32_t bufferSize = display.getBufferSize();
        logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, bufferSize);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || gpio.wasTouchActivity() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    if (gpio.isPressed(HalGPIO::BTN_POWER)) return;
    if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
      screenshotButtonsReleased = true;
      screenshotComboActive = false;
      return;
    }
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
  }

  // Consume the second X4 Pro power-button release so it does not also run a
  // configured short-power action after toggling the frontlight.
  if (handleX4ProFrontlightDoubleClick()) {
    return;
  }

  const bool x4ProDoubleClickPwrLight = BoardConfig::isX4Pro() && SETTINGS.doubleClickPwrLight;

#if FREEINK_CAP_TOUCH
  // A single X4 Pro power click becomes Confirm only after the frontlight
  // double-click window expires without a second click.
  mappedInputManager.setPowerConfirmClickFrame(false);
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PWR_CONFIRM && x4ProDoubleClickPwrLight) {
    if (lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
      lastX4ProPowerClickAt = 0;
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
    // A release held too long to be a double-click candidate (but still within
    // the normal Confirm press duration) never reaches handleX4ProFrontlightDoubleClick's
    // click tracking above, so it needs its own Confirm check here.
    if (mappedInputManager.wasReleased(MappedInputManager::Button::Power) &&
        gpio.getPowerButtonHeldTime() > X4PRO_POWER_CLICK_MAX_HOLD_MS &&
        gpio.getPowerButtonHeldTime() <= SETTINGS.getPowerButtonDuration()) {
      mappedInputManager.setPowerConfirmClickFrame(true);
    }
  }
#endif

  // Same deferral for SLEEP: getPowerButtonDuration() drops to 10ms so a quick
  // tap sleeps the device, which otherwise fires on button-down and never lets
  // a second click land. Sleep only once the double-click window has passed.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP && x4ProDoubleClickPwrLight &&
      lastX4ProPowerClickAt != 0 && millis() - lastX4ProPowerClickAt > X4PRO_POWER_DOUBLE_CLICK_MS) {
    lastX4ProPowerClickAt = 0;
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (deepSleepRequested) {
    deepSleepRequested = false;
    LOG_DBG("SLP", "Sleep requested by an activity");
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // A hold that woke the device must be released before it can count as a new
  // in-app long press. Otherwise a user who keeps holding after wake would put
  // the device straight back to sleep once allowSleepAt expires.
  static bool powerReleasedSinceWake = false;
  if (!gpio.isPressed(HalGPIO::BTN_POWER)) powerReleasedSinceWake = true;

  // On X4 Pro with SLEEP, a press still within the click window is a
  // double-click candidate — let it be released and evaluated above instead
  // of sleeping on button-down.
  const bool x4ProAwaitingClickWindow = x4ProDoubleClickPwrLight &&
                                        SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP &&
                                        gpio.getPowerButtonHeldTime() <= X4PRO_POWER_CLICK_MAX_HOLD_MS;

  if (!x4ProAwaitingClickWindow && powerReleasedSinceWake && millis() >= allowSleepAt &&
      gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getPowerButtonHeldTime() > SETTINGS.getPowerButtonDuration()) {
    // If the screenshot combination is potentially being pressed, don't sleep
    if (gpio.isPressed(HalGPIO::BTN_DOWN)) {
      return;
    }
    LOG_DBG("MAIN", "Power button held %lums, sleeping", gpio.getPowerButtonHeldTime());
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

#if FREEINK_DEVICE_PAPERMONO
  // Paper Mono reports the PMIC power button as a one-tick click, so the held
  // path above cannot fire. With the default Ignore action, retain the normal
  // power-button meaning and shut down; explicit alternate bindings still win.
  if ((SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP ||
       SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE) &&
      millis() >= allowSleepAt && mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    enterDeepSleep();
    return;
  }
#endif

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::ToggleFrontlight) {
    toggleFrontlight();
  }
  if (mappedInputManager.homeButtonAction() == HomeButtonAction::Refresh ||
      (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
       mappedInputManager.wasReleased(MappedInputManager::Button::Power))) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  // Not while reading: there a repaint is a full page re-render (visible
  // flash, the AA pass re-running, and a frontlight dip under the refresh
  // load); the reader's status bar picks the charging state up on the next
  // page turn instead.
  if (gpio.wasUsbStateChanged() && !activityManager.isReaderActivity()) {
    activityManager.requestUpdate();
  }

  // A step timer that ends while the reader is awake gets the same alert as one that ends while it sleeps.
  if (plaque::board().caps.stepTimers) startStepTimerAlertIfDue();

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  bool skipLoopDelay = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      // Let rendering advance without treating lock contention as idle.
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // Sleep in short slices and wake the poll as soon as a button contact closes.
      // InputManager commits a press only when two consecutive polls agree, so a
      // press shorter than one 50 ms sleep could land in a single sample and be lost.
      const unsigned long idleStart = millis();
      while (millis() - idleStart < 50) {
        delay(10);
        if (gpio.rawInputActive()) break;
      }
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
