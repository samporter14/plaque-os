#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_attr.h>

#include <cstring>
#include <string>
#include <utility>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "MethodDevice.h"
#include "ambient/AmbientSettings.h"
#include "ambient/LatestMessageProvider.h"
#include "ambient/MethodCheck.h"
#include "ambient/MethodLibrary.h"
#include "ambient/MethodWake.h"
#include "ambient/SleepFrame.h"
#include "ambient/StepTimerService.h"
#include "network/MessageTls.h"

// A timer wake's book check (Stage B). The decisions are in src/ambient/MethodWake.* and MethodCheck.*, covered by
// the host tests; this is the wiring: the device ports, the library read from the card, a loop that steps the check
// until it ends and looks at the hardware between steps, and the serial log of what was decided.
namespace {
// The book whose download failed on the wakes before this one, and how many wakes it has failed. RTC memory survives
// the deep sleeps between wakes (and nothing else: a reset or a flash starts the count again), so a download that
// cannot finish on this link is not tried again at the cost of the radio on every wake. See wake::Backoff.
RTC_DATA_ATTR uint32_t failedBook = 0;
RTC_DATA_ATTR uint8_t failedWakes = 0;

// The watch that tells the user from a phantom (see method::wake::PressWatch), and the capacitive Home key's level
// (the SDK reports that key as edges only).
method::wake::PressWatch pressWatch;
method::wake::HomeKeyLevel homeKey;

// Any contact down now: a finger on the screen, or the Home key. Call once after every gpio.update().
bool touchContact() {
  const bool homeDown =
      homeKey.observe(gpio.wasHomeKeyPressed(), gpio.wasHomeKeyTapped(), gpio.wasHomeKeyLongPressed());
  float x = 0;
  float y = 0;
  return gpio.isTouchHeldAt(x, y) || homeDown;
}

// When the wake last looked at the input (the update, not the end of the look), for lastLook().
uint32_t lastPollAt = 0;
bool polled = false;

// Did the user press a key or touch the reader? Read the way the main loop reads input, which setup() has already
// initialised (gpio.begin() sets up the buttons and the touch controller before the timer-wake branch): update()
// samples the button ladder, the power key and the touch controller and debounces. A key that has been held since
// before the first sample has no edge to report, so the level of the ladder and the power line is read as well, twice
// a few milliseconds apart (one noisy sample must not end a download). A touch counts only as a new contact that
// lasts: a reader face-down on a bench or in a pouch can report contact at every poll, and each would otherwise
// boot the reader and keep it awake. Nothing else of the input stack is needed.
bool userActed() {
  gpio.update();
  lastPollAt = millis();
  polled = true;
  const bool keyEdge = gpio.wasAnyPressed();
  const bool keyHeld = gpio.rawInputActive();
  bool keyHeldAgain = false;
  if (keyHeld) {
    delay(6);
    keyHeldAgain = gpio.rawInputActive();
  }
  return pressWatch.observe(millis(), keyEdge, keyHeld, keyHeldAgain, touchContact());
}

// Is a key other than power down (its debounced state)? On this board the side keys are plain digital inputs.
bool keyOtherThanPowerDown() {
  for (uint8_t key = HalGPIO::BTN_BACK; key < HalGPIO::BTN_POWER; ++key)
    if (gpio.isPressed(key)) return true;
  return false;
}

// Is any key down? The ladder and the power line by level, and every key by its debounced state.
bool anyKeyDown() { return gpio.rawInputActive() || gpio.isPressed(HalGPIO::BTN_POWER) || keyOtherThanPowerDown(); }

// The last look before the wake ends without a press. A key that has been held since the first look only commits at
// an update at least KEY_SETTLE_MS after it, which the early exits (a gate that said no, no memory) would otherwise
// not give it: they look again microseconds after the first look. And a finger that came down within the last
// TOUCH_CONFIRM_MS has not been seen for long enough to count yet: wait out the rest of that window (50 ms at most)
// and read once more, so that it is not lost.
bool lastLook() {
  const uint32_t now = millis();
  const uint32_t wait = pressWatch.lastLookWaitMs(now, polled ? now - lastPollAt : UINT32_MAX);
  if (wait > 0) delay(wait + 1);  // a tick may come up one millisecond short
  return userActed();
}
}  // namespace

namespace ambient {
bool checkBooksOnWake(GfxRenderer& renderer, const bool noteFailed) {
  using namespace method;
  const uint32_t elapsedMs = millis();  // since this wake began: millis() restarts at every wake
  const auto& settings = AMBIENT_SETTINGS;
  const bool https = std::strncmp(settings.baseUrl, "https://", 8) == 0;
  const int battery = powerManager.getBatteryPercentage();

  // A key held since the note check counts (its level is read, there is no edge to see): the user does not wait for a
  // poll they did not ask for. A finger already on the screen at this first look does not: it looks the same as a
  // phantom, and counts only once it has lifted and touched again.
  if (userActed()) {
    LOG_INF("METHOD", "Wake books: a press at %u ms, not checking", static_cast<unsigned>(elapsedMs));
    return true;
  }

  // Before anything is built or any request made: can this wake use the books at all? A step timer that is running
  // cuts the budget, so that a book never makes its alert late.
  const bool timerRunning = STEP_TIMER.running();
  const wake::Rules budget;
  const uint32_t leftMs = wake::budgetLeft(elapsedMs, timerRunning, timerRunning ? STEP_TIMER.remainingMs() : 0);
  const wake::Gate gate = wake::gate(budget, {battery, leftMs, https, message_tls::clockReady(), noteFailed});
  if (gate != wake::Gate::Run) {
    LOG_INF("METHOD", "Wake books: skipped, %s (battery %d%%, %u ms into the wake, %u ms of budget left%s)",
            wake::gateText(gate), battery, static_cast<unsigned>(elapsedMs), static_cast<unsigned>(leftMs),
            timerRunning ? ", a step timer is running" : "");
    return lastLook();
  }

  auto ports = makeUniqueNoThrow<DevicePorts>(renderer);
  auto library = makeUniqueNoThrow<Library>();
  if (!ports || !library) {
    LOG_ERR("METHOD", "Wake books: out of memory");
    return lastLook();
  }
  // Should the radio have dropped since the note check: the profile and the last-used network only, no scan.
  ports->wifi.setMode(netjoin::Mode::Wake);
  const auto report = library->load(ports->files);
  LOG_INF("METHOD", "Wake books: shelf %u books, mark %u (index read=%d dropped=%u adopted=%u saved=%d)",
          static_cast<unsigned>(library->entries().size()), static_cast<unsigned>(library->mark()), report.indexRead,
          static_cast<unsigned>(report.dropped), static_cast<unsigned>(report.adopted), report.saved);

  wake::Backoff backoff;
  backoff.bookId = failedBook;
  backoff.failures = failedWakes;
  wake::forgetHandled(backoff, library->mark());

  // The book a button wake would reopen is on the panel: filing a book never deletes it.
  const bool resumes = sleepframe::resumesBook(APP_STATE.openEpubPath, APP_STATE.lastSleepFromReader, false,
                                               APP_STATE.readerActivityLoadCount);
  const std::string protectedPath = resumes ? APP_STATE.openEpubPath : std::string();

  const uint32_t startedAt = millis();
  // The note check's time is already spent, and a running step timer's end is not to be missed.
  const uint32_t remainingMs = wake::budgetLeft(startedAt, timerRunning, timerRunning ? STEP_TIMER.remainingMs() : 0);
  Limits limits;
  limits.wake = wake::rulesFor(remainingMs, protectedPath, wake::blockedId(backoff));
  limits.maxDownloads = limits.wake.maxBooks;
  Config config;
  config.configured = settings.configured();
  config.https = https;
  config.deviceId = settings.deviceId;
  const Ports wiring{ports->clock, ports->wifi, ports->http, ports->files, ports->hash, ports->battery, &ports->log};
  auto check = makeUniqueNoThrow<Check>(wiring, std::move(config), *library, limits);
  if (!check) {
    LOG_ERR("METHOD", "Wake books: out of memory");
    return lastLook();
  }
  LOG_INF("METHOD", "Wake books: battery %d%%, %u of %u ms of the wake budget left, protecting %s", battery,
          static_cast<unsigned>(remainingMs), static_cast<unsigned>(wake::BUDGET_MS),
          protectedPath.empty() ? "nothing" : protectedPath.c_str());
  if (timerRunning)
    LOG_INF("METHOD", "Wake books: a step timer ends in %d ms: the budget leaves it %d ms of lead and %d ms of margin",
            static_cast<int>(STEP_TIMER.remainingMs()), static_cast<int>(steptimer::WAKE_LEAD_MS),
            static_cast<int>(wake::TIMER_MARGIN_MS));

  // Every phase has a time limit of its own and the check ends itself at the budget, so this loop ends; the hard stop
  // is for a check that somehow does not. The hardware is read after every step (each is tens of milliseconds, bar
  // the TLS handshake of a request, which the 5 s socket timeout bounds): a press cancels the check through its
  // normal teardown, which removes the partial file and turns the radio off, and the caller carries on as a button
  // wake. A press that began and ended inside one handshake is not seen; the user presses again.
  const uint32_t hardStopAt = startedAt + remainingMs + wake::hardStopGraceMs(timerRunning);
  bool pressed = false;
  check->start();
  while (check->running()) {
    check->step();
    if (check->running() && userActed()) {
      pressed = true;
      LOG_INF("METHOD", "Wake books: a press at %u ms, stopping the check", static_cast<unsigned>(millis()));
      check->cancel();
      break;
    }
    if (check->running() && static_cast<int32_t>(millis() - hardStopAt) > 0) {
      LOG_ERR("METHOD", "Wake books: hard stop");
      check->cancel();
    }
    delay(1);
  }
  // A press during the last step, or while the check was ending on its own, is still the user's.
  if (!pressed) pressed = lastLook();

  // What was decided about each book, in order.
  for (const auto& decision : check->decisions()) {
    char text[160];
    wake::describe(limits.wake, decision, text, sizeof(text));
    LOG_INF("METHOD", "Wake books: %s", text);
  }
  const uint32_t failed = check->failedBookId();
  if (failed != 0) {
    wake::noteFailure(backoff, failed);
    LOG_INF("METHOD", "Wake books: the download of b%u has failed on %u wake(s)", static_cast<unsigned>(failed),
            static_cast<unsigned>(backoff.failures));
  } else {
    wake::forgetHandled(backoff, library->mark());
  }
  failedBook = backoff.bookId;
  failedWakes = backoff.failures;
  LOG_INF("METHOD", "Wake books: finished, result=%d filed=%u mark=%u more_waiting=%d in %u ms (free=%u max_block=%u)",
          static_cast<int>(check->result()), static_cast<unsigned>(check->filedCount()),
          static_cast<unsigned>(library->mark()), check->moreWaiting(), static_cast<unsigned>(millis() - startedAt),
          static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  return pressed;
}

void settleWakePress(MappedInputManager& input) {
  // The press that ended the wake is the wake gesture, as the power key's press is on a button wake. A tap is over in
  // well under a second; wait for it (the boot below takes longer than that anyway) so that its release is not read as
  // an action by the screen the boot opens: a Back release leaving the book, or a capacitive Home key's release read as
  // a tap. The keys, a finger on the screen and the Home key are all waited for. On this board the side keys are plain
  // digital inputs, which rawInputActive() (the ladder and the power line) does not read, so every key's debounced
  // state is read as well. Then take what is still held into the input state as a non-edge, as the silent reboot does.
  const uint32_t until = millis() + method::wake::RELEASE_WAIT_MS;
  while (static_cast<int32_t>(millis() - until) < 0) {
    gpio.update();
    const bool touching = touchContact();  // also follows the Home key's edges
    if (!anyKeyDown() && !touching) break;
    delay(5);
  }
  gpio.update();
  delay(10);
  gpio.update();

  // Whatever is still down now outlasted the wait: it was a hold. Its release must not act, so it is treated as the app
  // treats a consumed long-press (the release is swallowed). The home key needs nothing here: the SDK's long press has
  // fired for it by now and its release is no tap.
  float x = 0;
  float y = 0;
  const method::wake::Leftover left = method::wake::leftover(gpio.isTouchHeldAt(x, y), keyOtherThanPowerDown());
  if (left.suppressTouch) gpio.suppressTouchContact();
  if (left.consumeKeyReleases) {
    using Button = MappedInputManager::Button;
    for (const Button button : {Button::Back, Button::Confirm, Button::Left, Button::Right, Button::Up, Button::Down})
      input.wasLongPressed(button, 0);  // a no-op for a key that is up
  }
}
}  // namespace ambient
