#include "PlaqueBoard.h"

namespace plaque {

namespace {
// What every Plaque reader offers, whatever its hardware.
Capabilities plaqueFeatures() {
  Capabilities c;
  c.keys = true;
  c.method = true;
  c.mailbox = true;
  c.notes = true;
  c.almanac = true;
  c.registerScreen = true;
  c.scheduledDownloads = true;
  c.timedWake = true;
  c.stepTimers = true;
  c.cardIdentity = true;
  c.usbDrive = true;
  c.usbSetup = true;
  c.wifiOpen = true;
  c.wifiPsk = true;
  c.wifiEapTls = true;
  return c;
}
}  // namespace

Model modelOf(const BoardFacts& facts) {
  if (facts.x4Pro == facts.x4Classic) return Model::Unsupported;
  return facts.x4Pro ? Model::X4Pro : Model::X4Classic;
}

Capabilities capabilitiesFor(const Model model) {
  switch (model) {
    case Model::X4Pro: {
      Capabilities c = plaqueFeatures();
      c.touch = true;
      c.homeKey = true;
      c.frontlight = true;
      c.alertLight = true;
      return c;
    }
    case Model::X4Classic:
      // Side page keys, power and four bottom keys; no touchscreen, no Home key, no frontlight.
      return plaqueFeatures();
    case Model::Unsupported:
      break;
  }
  return Capabilities();
}

Board boardFor(const BoardFacts& facts) {
  Board board;
  board.model = modelOf(facts);
  if (board.model == Model::Unsupported) {
    // Today's behaviour: no Plaque feature, the hardware as the profile has it.
    board.caps.keys = true;
    board.caps.touch = facts.touch;
    board.caps.homeKey = facts.homeKey;
    board.caps.frontlight = facts.frontlight;
    board.caps.usbDrive = facts.usbDrive;
    return board;
  }
  Capabilities c = capabilitiesFor(board.model);
  c.touch = c.touch && facts.touch;
  c.homeKey = c.homeKey && facts.homeKey;
  c.frontlight = c.frontlight && facts.frontlight;
  c.alertLight = c.alertLight && c.frontlight;
  c.usbDrive = c.usbDrive && facts.usbDrive;
  board.caps = c;
  return board;
}

const char* modelId(const Model model) {
  switch (model) {
    case Model::X4Pro:
      return "x4pro";
    case Model::X4Classic:
      return "x4c";
    case Model::Unsupported:
      break;
  }
  return "";
}

const char* modelName(const Model model) {
  switch (model) {
    case Model::X4Pro:
      return "Xteink X4 Pro";
    case Model::X4Classic:
      return "Xteink X4 Classic";
    case Model::Unsupported:
      break;
  }
  return "";
}

Tokens capabilityTokens(const Capabilities& caps) {
  Tokens out;
  const auto add = [&out](const bool on, const char* token) {
    if (!on) return;
    if (out.count < MAX_TOKENS) {
      out.items[out.count++] = token;
    } else {
      out.overflowed = true;
    }
  };
  add(caps.keys, "keys");
  add(caps.touch, "touch");
  add(caps.frontlight, "frontlight");
  add(caps.method, "method");
  add(caps.notes, "notes");
  add(caps.almanac, "almanac");
  add(caps.stepTimers, "step_timers");
  add(caps.timedWake, "timed_wake");
  add(caps.scheduledDownloads, "scheduled_downloads");
  add(caps.usbDrive, "usb_drive");
  add(caps.usbSetup, "usb_setup");
  add(caps.wifiOpen, "wifi_open");
  add(caps.wifiPsk, "wifi_psk");
  add(caps.wifiEapTls, "wifi_eap_tls");
  // Added after the network tokens, so every earlier token keeps its name and its place: a reader that predates it, and
  // a Mac that does not know it, both read the rest of the list as before (docs/SPEC-UNIVERSITY-WIFI.md section 2).
  add(offersRegister(caps), "register");
  return out;
}

}  // namespace plaque
