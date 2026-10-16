#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "MethodWake.h"
#include "PlaqueBoard.h"
#include "SleepFrame.h"
#include "StepTimer.h"

// Host tests for the capability layer (lib/PlaqueBoard): what the X4 Pro, the X4 Classic and an unsupported board
// offer, the reader.json tokens, and the decisions the firmware makes from them, walked through the same pure code
// main.cpp and the step timer use: the wake timer each board arms, the alert without a frontlight, the Method sleep
// that keeps the step, and a key press during a timer wake on a reader with keys only.

using plaque::Board;
using plaque::BoardFacts;
using plaque::Capabilities;
using plaque::Model;

namespace {
int checks = 0;

void check(const bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

BoardFacts proFacts() {
  BoardFacts f;
  f.x4Pro = true;
  f.touch = true;
  f.homeKey = true;
  f.frontlight = true;
  f.usbDrive = true;
  return f;
}

BoardFacts classicFacts() {
  BoardFacts f;
  f.x4Classic = true;
  f.usbDrive = true;
  return f;
}

std::vector<std::string> tokens(const Capabilities& caps) {
  const plaque::Tokens t = plaque::capabilityTokens(caps);
  return std::vector<std::string>(t.items, t.items + t.count);
}

bool allPlaqueFeatures(const Capabilities& c) {
  return c.keys && c.method && c.mailbox && c.notes && c.almanac && c.scheduledDownloads && c.timedWake &&
         c.registerScreen && c.stepTimers && c.cardIdentity && c.usbDrive && c.usbSetup && c.wifiOpen && c.wifiPsk &&
         c.wifiEapTls;
}

bool noPlaqueFeature(const Capabilities& c) {
  return !c.method && !c.mailbox && !c.notes && !c.almanac && !c.registerScreen && !c.scheduledDownloads &&
         !c.timedWake && !c.stepTimers && !c.alertLight && !c.cardIdentity && !c.usbSetup && !c.wifiOpen &&
         !c.wifiPsk && !c.wifiEapTls;
}

void testModels() {
  check(plaque::modelOf(proFacts()) == Model::X4Pro, "the X4 Pro's profile is the X4 Pro");
  check(plaque::modelOf(classicFacts()) == Model::X4Classic, "the X4 Classic's profile is the X4 Classic");
  check(plaque::modelOf(BoardFacts{}) == Model::Unsupported, "any other board is unsupported");
  BoardFacts both = proFacts();
  both.x4Classic = true;
  check(plaque::modelOf(both) == Model::Unsupported, "a profile claiming both boards is not trusted");

  check(std::strcmp(plaque::modelId(Model::X4Pro), "x4pro") == 0, "model id x4pro");
  check(std::strcmp(plaque::modelId(Model::X4Classic), "x4c") == 0, "model id x4c");
  check(std::strcmp(plaque::modelId(Model::Unsupported), "") == 0, "no model id for an unsupported board");
  check(std::strcmp(plaque::modelName(Model::X4Pro), "Xteink X4 Pro") == 0, "model name of the Pro");
  check(std::strcmp(plaque::modelName(Model::X4Classic), "Xteink X4 Classic") == 0, "model name of the Classic");
  check(std::strcmp(plaque::modelName(Model::Unsupported), "") == 0, "no model name for an unsupported board");
  check(std::strcmp(plaque::WIFI_BAND_24GHZ, "2.4GHz") == 0, "the one band");
}

void testCapabilityTable() {
  const Capabilities pro = plaque::capabilitiesFor(Model::X4Pro);
  check(pro.touch && pro.homeKey && pro.frontlight && pro.alertLight, "the Pro has touch, Home and a light it pulses");
  check(allPlaqueFeatures(pro), "the Pro has every Plaque feature");

  const Capabilities classic = plaque::capabilitiesFor(Model::X4Classic);
  check(!classic.touch && !classic.homeKey && !classic.frontlight && !classic.alertLight,
        "the Classic has no touch, no Home key, no light");
  check(allPlaqueFeatures(classic), "the Classic has every Plaque feature, on its keys");

  const Capabilities none = plaque::capabilitiesFor(Model::Unsupported);
  check(noPlaqueFeature(none) && !none.usbDrive && !none.touch, "an unsupported model offers nothing");
}

void testBoardFromFacts() {
  const Board pro = plaque::boardFor(proFacts());
  check(pro.model == Model::X4Pro && pro.caps.touch && pro.caps.frontlight && pro.caps.alertLight, "the Pro as built");

  // A build without a driver offers nothing that needs it (fail safe), whatever the model's table says.
  BoardFacts noLight = proFacts();
  noLight.frontlight = false;
  const Board dark = plaque::boardFor(noLight);
  check(!dark.caps.frontlight && !dark.caps.alertLight, "no light driver: no light, no light in the alert");
  check(dark.caps.stepTimers && dark.caps.method, "the rest of Plaque stays");
  BoardFacts noTouch = proFacts();
  noTouch.touch = false;
  noTouch.homeKey = false;
  check(!plaque::boardFor(noTouch).caps.touch && !plaque::boardFor(noTouch).caps.homeKey, "no touch driver: no touch");
  BoardFacts noMsc = classicFacts();
  noMsc.usbDrive = false;
  check(!plaque::boardFor(noMsc).caps.usbDrive, "no USB mass storage in the build: no USB Drive");

  // The model's table caps what the profile reports: a Classic profile that claimed a frontlight is still a Classic.
  BoardFacts odd = classicFacts();
  odd.touch = true;
  odd.frontlight = true;
  const Board classic = plaque::boardFor(odd);
  check(
      classic.model == Model::X4Classic && !classic.caps.touch && !classic.caps.frontlight && !classic.caps.alertLight,
      "the Classic never offers touch or a light");

  // Other boards: the hardware as the profile has it, and no Plaque feature (exactly today's behaviour).
  BoardFacts sticky;
  sticky.touch = true;
  sticky.usbDrive = false;
  const Board other = plaque::boardFor(sticky);
  check(other.model == Model::Unsupported, "a touch board that is not an X4 is unsupported");
  check(other.caps.touch && !other.caps.homeKey && !other.caps.frontlight, "its hardware bits are its own");
  check(noPlaqueFeature(other.caps), "and no Plaque feature runs on it");
  BoardFacts c3;
  check(noPlaqueFeature(plaque::boardFor(c3).caps) && !plaque::boardFor(c3).caps.touch, "nor on a C3 X4");
}

void testTokens() {
  const std::vector<std::string> classic = tokens(plaque::capabilitiesFor(Model::X4Classic));
  // docs/SPEC-UNIVERSITY-WIFI.md section 2: the example file of an X4 Classic.
  const std::vector<std::string> contract = {
      "keys",      "method",    "notes",     "almanac",  "step_timers", "timed_wake", "scheduled_downloads",
      "usb_drive", "usb_setup", "wifi_open", "wifi_psk", "wifi_eap_tls"};
  // Register's token comes after all of them (additive: the others keep their names and their places).
  std::vector<std::string> withRegister = contract;
  withRegister.push_back("register");
  check(classic == withRegister, "the Classic's tokens are the contract's example, in its order, then register");

  const std::vector<std::string> pro = tokens(plaque::capabilitiesFor(Model::X4Pro));
  std::vector<std::string> expectedPro = withRegister;
  expectedPro.insert(expectedPro.begin() + 1, {"touch", "frontlight"});
  check(pro == expectedPro, "the Pro's tokens add touch and frontlight after keys");
  check(std::vector<std::string>(classic.begin(), classic.end() - 1) == contract,
        "dropping register leaves the contract's list untouched");

  check(tokens(plaque::capabilitiesFor(Model::Unsupported)).empty(), "an unsupported model has none");

  const std::regex token("^[a-z0-9_]+$");
  for (const auto& list : {classic, pro}) {
    std::set<std::string> seen;
    for (const std::string& t : list) {
      check(std::regex_match(t, token), "token " + t + " is [a-z0-9_]");
      check(seen.insert(t).second, "token " + t + " is listed once");
    }
    check(list.size() <= plaque::MAX_TOKENS, "within MAX_TOKENS");
  }
  for (const std::string& t : classic)
    check(t != "touch" && t != "frontlight" && t != "home_key", "the Classic never lists " + t);

  // A Pro build without its light driver does not claim a frontlight.
  BoardFacts noLight = proFacts();
  noLight.frontlight = false;
  for (const std::string& t : tokens(plaque::boardFor(noLight).caps)) check(t != "frontlight", "no light, no token");
}

// The network features a screen offers, asked by capability: both Plaque boards have all of them, any other board none.
void testNetworkFeatures() {
  const Capabilities pro = plaque::capabilitiesFor(Model::X4Pro);
  const Capabilities classic = plaque::capabilitiesFor(Model::X4Classic);
  const Capabilities other = plaque::capabilitiesFor(Model::Unsupported);
  for (const Capabilities& caps : {pro, classic}) {
    check(plaque::offersNetworkProfile(caps), "the Network screen and an installed profile");
    check(plaque::writesProfileSummary(caps), "/Lamina/network-profile.json");
    check(plaque::offersCardImport(caps), "Import from card");
    check(plaque::offersConnectToMac(caps), "Connect to Mac");
    check(plaque::offersConnectionCheck(caps), "Check connection");
    for (const auto kind : {plaque::WifiKind::Open, plaque::WifiKind::Psk, plaque::WifiKind::EapTls})
      check(plaque::supportsWifi(caps, kind), "every kind of network a profile carries");
  }
  // A C3 X4, a Sticky, a Paper Mono: none of it, whatever their hardware reports.
  BoardFacts sticky;
  sticky.touch = true;
  sticky.usbDrive = true;
  for (const Capabilities& caps : {other, plaque::boardFor(BoardFacts{}).caps, plaque::boardFor(sticky).caps}) {
    check(!plaque::offersNetworkProfile(caps), "no network profile on another board");
    check(!plaque::writesProfileSummary(caps), "no /Lamina/network-profile.json on another board");
    check(!plaque::offersCardImport(caps), "no Import from card");
    check(!plaque::offersConnectToMac(caps), "no Connect to Mac");
    check(!plaque::offersConnectionCheck(caps), "no Check connection");
    for (const auto kind : {plaque::WifiKind::Open, plaque::WifiKind::Psk, plaque::WifiKind::EapTls})
      check(!plaque::supportsWifi(caps, kind), "and no kind of network");
  }

  // Each feature asks for its own capability: take one away and only what depends on it goes.
  Capabilities noSetup = classic;
  noSetup.usbSetup = false;
  check(!plaque::offersConnectToMac(noSetup) && plaque::offersCardImport(noSetup) &&
            plaque::offersNetworkProfile(noSetup),
        "without the USB setup link there is still the card and the screen");
  Capabilities pskOnly = classic;
  pskOnly.wifiOpen = false;
  pskOnly.wifiEapTls = false;
  check(
      plaque::offersNetworkProfile(pskOnly) && plaque::offersCardImport(pskOnly) && plaque::offersConnectToMac(pskOnly),
      "one kind of network is enough for a profile");
  check(plaque::supportsWifi(pskOnly, plaque::WifiKind::Psk) &&
            !plaque::supportsWifi(pskOnly, plaque::WifiKind::EapTls) &&
            !plaque::supportsWifi(pskOnly, plaque::WifiKind::Open),
        "and only that kind is accepted");
  Capabilities eapOnly = classic;
  eapOnly.wifiOpen = false;
  eapOnly.wifiPsk = false;
  check(plaque::offersNetworkProfile(eapOnly) && !plaque::offersCardImport(eapOnly),
        "a certificate-only board has no card import: a key never travels on the card");
  Capabilities noMailbox = classic;
  noMailbox.mailbox = false;
  check(!plaque::offersConnectionCheck(noMailbox) && plaque::offersNetworkProfile(noMailbox),
        "the check is the mailbox's; the profile does not need one");
  Capabilities noIdentity = classic;
  noIdentity.cardIdentity = false;
  check(!plaque::writesProfileSummary(noIdentity) && plaque::offersNetworkProfile(noIdentity),
        "the summary file lives beside reader.json and goes with it");
}

void testPrompts() {
  const Capabilities pro = plaque::capabilitiesFor(Model::X4Pro);
  const Capabilities classic = plaque::capabilitiesFor(Model::X4Classic);
  check(plaque::promptsTouch(pro), "the Pro's prompts name a tap (\"Tap to stop\")");
  check(!plaque::promptsTouch(classic), "the Classic's name a key (\"Press Confirm to stop\", \"Press any key\")");
  check(plaque::ambientSleepScreen(pro) && plaque::ambientSleepScreen(classic), "Notes and Almanac on both");
  check(!plaque::ambientSleepScreen(plaque::capabilitiesFor(Model::Unsupported)), "not elsewhere");
}

// Register (an instrument's schedule) is offered wherever Notes is: the sleep screen, timed wake and the mailbox.
void testRegister() {
  const Capabilities pro = plaque::capabilitiesFor(Model::X4Pro);
  const Capabilities classic = plaque::capabilitiesFor(Model::X4Classic);
  check(plaque::offersRegister(pro) && plaque::offersRegister(classic), "Register on both boards");
  check(plaque::offersRegister(pro) == plaque::ambientSleepScreen(pro) &&
            plaque::offersRegister(classic) == plaque::ambientSleepScreen(classic),
        "the same boards as Notes");
  check(!plaque::offersRegister(plaque::capabilitiesFor(Model::Unsupported)), "not on another board");
  check(!plaque::offersRegister(plaque::boardFor(BoardFacts{}).caps), "not on a C3 X4");
  BoardFacts sticky;
  sticky.touch = true;
  sticky.usbDrive = true;
  check(!plaque::offersRegister(plaque::boardFor(sticky).caps), "not on a Sticky or a Paper Mono");
  // The Pro as built and the Classic as built both have it.
  check(plaque::offersRegister(plaque::boardFor(proFacts()).caps) &&
            plaque::offersRegister(plaque::boardFor(classicFacts()).caps),
        "the boards as the profiles report them");
  // A build without the touch or light driver is still a Register reader: Register needs none of the hardware bits.
  BoardFacts noLight = proFacts();
  noLight.frontlight = false;
  noLight.touch = false;
  noLight.homeKey = false;
  check(plaque::offersRegister(plaque::boardFor(noLight).caps), "Register needs no touch or light");

  // Each thing it needs, taken away on its own.
  Capabilities c = classic;
  c.registerScreen = false;
  check(!plaque::offersRegister(c), "without the bit");
  c = classic;
  c.mailbox = false;
  check(!plaque::offersRegister(c), "without the mailbox there is nothing to ask");
  c = classic;
  c.timedWake = false;
  check(!plaque::offersRegister(c), "without timed wake the screen could not move on");
  c = classic;
  c.notes = false;
  check(plaque::offersRegister(c), "Almanac alone still gives the sleep screen");
  c.almanac = false;
  check(!plaque::offersRegister(c), "without a sleep screen there is none to draw");
  // The token follows the helper, not the bit alone.
  c = classic;
  c.mailbox = false;
  const auto t = tokens(c);
  check(std::find(t.begin(), t.end(), "register") == t.end(), "no register token where Register is not offered");
}

// The token list has a fixed capacity and drops what does not fit: that must never happen silently. With every
// capability on, the list fits (and says so); a list one over would be reported as overflowed.
void testTokenCapacity() {
  Capabilities all;
  all.keys = all.touch = all.homeKey = all.frontlight = true;
  all.method = all.mailbox = all.notes = all.almanac = all.registerScreen = all.scheduledDownloads = true;
  all.timedWake = all.stepTimers = all.alertLight = all.cardIdentity = all.usbDrive = all.usbSetup = true;
  all.wifiOpen = all.wifiPsk = all.wifiEapTls = true;
  const plaque::Tokens full = plaque::capabilityTokens(all);
  check(!full.overflowed, "every token fits in MAX_TOKENS (a 17th would be dropped: raise MAX_TOKENS)");
  check(full.count == 15 && full.count <= plaque::MAX_TOKENS,
        "fifteen tokens today, within the capacity of " + std::to_string(plaque::MAX_TOKENS));
  for (const Model model : {Model::X4Pro, Model::X4Classic, Model::Unsupported})
    check(!plaque::capabilityTokens(plaque::capabilitiesFor(model)).overflowed, "no board's list overflows");
}

// Reset the clock check is offered wherever the clock floor matters, whatever the RTC probe found.
void testClockReset() {
  check(plaque::offersClockReset(plaque::capabilitiesFor(Model::X4Pro)) &&
            plaque::offersClockReset(plaque::capabilitiesFor(Model::X4Classic)),
        "on both Plaque boards");
  check(!plaque::offersClockReset(plaque::capabilitiesFor(Model::Unsupported)), "not on another board");
  Capabilities c = plaque::capabilitiesFor(Model::X4Classic);
  c.mailbox = false;
  check(plaque::offersClockReset(c), "a certificate network alone needs it");
  c.wifiOpen = c.wifiPsk = c.wifiEapTls = false;
  check(!plaque::offersClockReset(c), "with neither the mailbox nor a profile there is no use of a trusted time");
}

// The wake timer the reader arms on the way to sleep: the step timer's end, sooner than the poll.
void testWakePlanning() {
  const Capabilities pro = plaque::capabilitiesFor(Model::X4Pro);
  const Capabilities classic = plaque::capabilitiesFor(Model::X4Classic);
  const Capabilities other = plaque::capabilitiesFor(Model::Unsupported);
  const uint64_t pollUs = 60ULL * 60 * 1000000;  // an hourly poll
  const int64_t leftMs = 5 * 60 * 1000;          // "Start 5:00 timer", just started
  const uint64_t wantUs = steptimer::nextWakeMicros(pollUs, true, leftMs);
  check(wantUs == static_cast<uint64_t>(leftMs - steptimer::WAKE_LEAD_MS) * 1000, "the timer's end comes first");
  check(plaque::armedWakeMicros(classic, wantUs) == wantUs, "the Classic arms the step timer's wake");
  check(plaque::armedWakeMicros(pro, wantUs) == wantUs, "the Pro arms it as before");
  check(plaque::armedWakeMicros(other, wantUs) == 0, "another board never arms a timer");
  check(plaque::armedWakeMicros(classic, 0) == 0, "nothing asked, nothing armed");
  check(plaque::armedWakeMicros(classic, pollUs) == pollUs, "the Classic arms the scheduled poll");

  // The wake at the end is the alert, on both boards (the same rule).
  const steptimer::BootFacts atEnd{steptimer::Phase::Running, false, 1500, true};
  check(steptimer::planBoot(atEnd) == steptimer::BootAction::AlertPulse, "the timer wake is the alert");
  // A poll that would run into the end is skipped on any board with timed wake.
  check(!steptimer::pollFitsBeforeEnd(true, 60 * 1000), "a poll a minute before the end waits");
}

// The alert on a board without a frontlight: the light is never asked for, and the alert still holds the reader awake
// to its limit for a press.
void testAlertWithoutLight() {
  const Capabilities classic = plaque::capabilitiesFor(Model::X4Classic);
  const Capabilities pro = plaque::capabilitiesFor(Model::X4Pro);
  check(!plaque::alertHasLight(classic, false), "the Classic has no light to pulse");
  check(!plaque::alertHasLight(classic, true), "even if a driver answered");
  check(plaque::alertHasLight(pro, true), "the Pro pulses its light");
  check(!plaque::alertHasLight(pro, false), "but not one whose driver is absent");

  // StepTimerAlertActivity::stepLight: quiet = !light allowed.
  const bool quiet = !plaque::alertHasLight(classic, false);
  bool everOn = false;
  for (int64_t t = 0; t < steptimer::ALERT_LIMIT_MS; t += 50) everOn = everOn || steptimer::alertLight(t, quiet).on;
  check(!everOn, "no frame of the Classic's alert switches a light on");
  check(!steptimer::alertLight(steptimer::ALERT_LIMIT_MS - 1, quiet).ended, "it waits for a press to the limit");
  check(steptimer::alertLight(steptimer::ALERT_LIMIT_MS, quiet).ended, "and ends there, as on the Pro");
  // The Pro's light still flashes.
  bool proOn = false;
  for (int64_t t = 0; t < 3000; t += 50) proOn = proOn || steptimer::alertLight(t, false).on;
  check(proOn, "the Pro's alert lights up");
}

// Sleeping in a Method book keeps the step on the panel, and a timer wake does not draw over it, on the Classic too.
void testMethodSleep() {
  const std::string book = "/Lamina/Method/Example column clean-up 0123abcd.xtc";
  for (const Model model : {Model::X4Pro, Model::X4Classic}) {
    const Capabilities caps = plaque::capabilitiesFor(model);
    const sleepframe::SleepPlan plan = sleepframe::planSleep({caps.method, true, book, false, true});
    check(plan.frame == sleepframe::Frame::QuickResume && plan.methodStep,
          std::string(plaque::modelId(model)) + ": a Method book sleeps as Quick Resume, marker set");
    const sleepframe::WakePlan timer = sleepframe::planWake(sleepframe::Wake::Timer, plan.methodStep, true);
    check(!timer.redraw && timer.commitFetched && timer.markerAfter,
          std::string(plaque::modelId(model)) + ": a timer wake keeps the step and saves the note");
    const sleepframe::WakePlan button = sleepframe::planWake(sleepframe::Wake::Button, timer.markerAfter, false);
    check(!button.markerAfter, std::string(plaque::modelId(model)) + ": a button wake clears the marker");
  }
  // Other boards keep their own sleep screen (methodEnabled false), as before.
  const Capabilities other = plaque::capabilitiesFor(Model::Unsupported);
  const sleepframe::SleepPlan plan = sleepframe::planSleep({other.method, true, book, false, false});
  check(plan.frame == sleepframe::Frame::Normal && !plan.methodStep, "another board: the normal sleep screen");
}

// A key on the Classic is how the user stops a timer wake's book check: no touch is ever reported there.
void testKeysDuringWake() {
  method::wake::PressWatch watch;
  check(!watch.observe(0, false, false, false, false), "nothing pressed");
  check(watch.observe(20, true, false, false, false), "a key's press edge is the user, with no touch at all");
  const method::wake::Ending end = method::wake::ending(true, true);
  check(end.continueBoot && end.sleepWake && end.commitNote && end.swallowRelease,
        "the wake becomes a button wake and keeps the note");
  method::wake::PressWatch held;
  check(held.observe(0, false, true, true, false), "a key held through two readings is the user");
  const method::wake::Leftover left = method::wake::leftover(false, true);
  check(!left.suppressTouch && left.consumeKeyReleases, "a key still down is swallowed, not acted on");
}
}  // namespace

int main() {
  testModels();
  testCapabilityTable();
  testBoardFromFacts();
  testTokens();
  testNetworkFeatures();
  testPrompts();
  testRegister();
  testTokenCapacity();
  testClockReset();
  testWakePlanning();
  testAlertWithoutLight();
  testMethodSleep();
  testKeysDuringWake();
  std::cout << "plaque board: " << checks << " checks passed\n";
  return 0;
}
