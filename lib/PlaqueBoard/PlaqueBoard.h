#pragma once

#include <cstddef>
#include <cstdint>

// What the board is, and what Plaque offers on it (docs/x4c-support.md). The model says which reader this is; the
// capabilities say what it has (touch, a Home key, a frontlight) and which Plaque features run on it (Method, the
// mailbox, Notes and Almanac, scheduled downloads, timed wake, step timers, the card's copy of the identity).
// Features are asked for by capability, never by model: a screen asks "is there a touchscreen?", not "is this an
// X4 Pro?". Only genuinely board-specific hardware paths (the X4 Pro's power double-click for the light, the GPIO0
// boot strap) still ask the SDK for the board itself.
//
// Everything here except board() is pure, covered by test/plaque_board. board() (PlaqueBoardDevice.cpp) reads the
// SDK's active profile. Boards other than the X4 Pro and X4 Classic are Model::Unsupported: every Plaque feature is
// off there, and their hardware bits are the profile's own.
namespace plaque {

enum class Model : uint8_t { Unsupported, X4Pro, X4Classic };

struct Capabilities {
  // The hardware.
  bool keys = false;        // physical keys reach every Plaque action
  bool touch = false;       // a touchscreen
  bool homeKey = false;     // the capacitive Home key below the panel
  bool frontlight = false;  // a frontlight Plaque may switch
  // Plaque on it.
  bool method = false;              // the Method shelf and its books (protocols, tables, plate maps, boards)
  bool mailbox = false;             // Settings › System › Messages, the identity headers, targeted delivery
  bool notes = false;               // the Notes sleep screen
  bool almanac = false;             // the Almanac sleep screen
  bool registerScreen = false;      // the Register sleep screen: an instrument's schedule (docs/SPEC-REGISTER.md)
  bool scheduledDownloads = false;  // timer wakes that fetch the note and new books, bounded and cancellable
  bool timedWake = false;           // deep sleep may arm a wake timer
  bool stepTimers = false;          // step timers and their alert
  bool alertLight = false;          // the step-timer alert pulses the frontlight
  bool cardIdentity = false;        // /Lamina/reader.json, rewritten at boot when it differs
  bool usbDrive = false;            // File Transfer › USB Drive
  bool usbSetup = false;            // network setup over the USB serial link
  bool wifiOpen = false;            // open networks
  bool wifiPsk = false;             // WPA/WPA2 personal
  bool wifiEapTls = false;          // WPA2-Enterprise with a client certificate
};

// What the SDK's active profile says about the board this firmware runs on.
struct BoardFacts {
  bool x4Pro = false;
  bool x4Classic = false;
  bool touch = false;
  bool homeKey = false;
  bool frontlight = false;
  bool usbDrive = false;  // the build has USB mass storage (FREEINK_CAP_USB_MSC)
};

struct Board {
  Model model = Model::Unsupported;
  Capabilities caps;
};

// The model of the board the facts describe. A profile that claims to be both is not one Plaque knows.
Model modelOf(const BoardFacts& facts);
// What the model has and what Plaque offers on it, as designed (docs/x4c-support.md, the capability table).
Capabilities capabilitiesFor(Model model);
// The model's capabilities, with each hardware bit (and what depends on it) kept only where the profile reports the
// hardware too: a build without the touch or frontlight driver offers neither. An unsupported board reports its
// profile's hardware and no Plaque feature.
Board boardFor(const BoardFacts& facts);

// "x4pro", "x4c"; empty for an unsupported board.
const char* modelId(Model model);
// "Xteink X4 Pro", "Xteink X4 Classic"; empty for an unsupported board.
const char* modelName(Model model);

// The Wi-Fi bands the reader's radio has (ESP32-S3: 2.4 GHz only), as reader.json writes them.
constexpr char WIFI_BAND_24GHZ[] = "2.4GHz";

// The capability tokens of reader.json ([a-z0-9_]), in the order the file lists them. `touch` and `frontlight` are
// listed only where the board has them.
constexpr size_t MAX_TOKENS = 16;
struct Tokens {
  const char* items[MAX_TOKENS] = {};
  size_t count = 0;
  bool overflowed = false;  // a token did not fit and was left out: MAX_TOKENS has to grow (test/plaque_board fails)
};
Tokens capabilityTokens(const Capabilities& caps);

// ---- decisions made from the capabilities ------------------------------------------------------------------------

// A screen's prompt names a tap only where there is a touchscreen; elsewhere it names the key.
constexpr bool promptsTouch(const Capabilities& caps) { return caps.touch; }
// The mailbox frame (Notes, Almanac) may be the sleep screen.
constexpr bool ambientSleepScreen(const Capabilities& caps) { return caps.notes || caps.almanac; }
// Register (an instrument's schedule) is a mailbox frame like Notes and Almanac, drawn as the sleep screen and
// refreshed by timer wakes, so it needs all of what they need: the sleep screen, timed wake and the mailbox. A board
// that offers those and has the bit offers Register; nothing asks the model.
constexpr bool offersRegister(const Capabilities& caps) {
  return caps.registerScreen && caps.mailbox && caps.timedWake && (caps.notes || caps.almanac);
}
// The wake timer deep sleep arms for `requestedMicros` (zero: none). Boards without timed wake never arm one.
constexpr uint64_t armedWakeMicros(const Capabilities& caps, uint64_t requestedMicros) {
  return caps.timedWake ? requestedMicros : 0;
}
// Whether the step-timer alert has a light to pulse: only a board Plaque alerts with its frontlight (the X4 Pro), and
// only when the light's driver answers. Everywhere else the inverted box is the whole alert and no light is touched.
constexpr bool alertHasLight(const Capabilities& caps, bool lightPresent) { return caps.alertLight && lightPresent; }

// ---- which network features a board offers
// ---------------------------------------------------------------------------
//
// The network profile and its screens are a Plaque feature like the others: a board that is not an X4 Pro or X4 Classic
// has none of them (every capability below is off there), and each screen asks for the capability it needs rather than
// the model.

// The kinds of network a profile can describe, as the capabilities name them.
enum class WifiKind : uint8_t { Open, Psk, EapTls };
constexpr bool supportsWifi(const Capabilities& caps, WifiKind kind) {
  switch (kind) {
    case WifiKind::Open:
      return caps.wifiOpen;
    case WifiKind::Psk:
      return caps.wifiPsk;
    case WifiKind::EapTls:
      return caps.wifiEapTls;
  }
  return false;
}
// A network profile can be installed at all: the board supports at least one kind of network a profile carries. Gates
// Settings > System > Network, the installed profile (it is not joined, shown or described on a board without this),
// the profile's time servers, and /Lamina/network-profile.json (which also needs the card's copy of the identity, next
// to which it lives).
constexpr bool offersNetworkProfile(const Capabilities& caps) {
  return caps.wifiOpen || caps.wifiPsk || caps.wifiEapTls;
}
constexpr bool writesProfileSummary(const Capabilities& caps) {
  return offersNetworkProfile(caps) && caps.cardIdentity;
}
// Import from card: the bundle on the card carries open and PSK profiles only (a key never travels on the card).
constexpr bool offersCardImport(const Capabilities& caps) { return caps.wifiOpen || caps.wifiPsk; }
// Connect to Mac: the USB setup link, which exists to install a profile.
constexpr bool offersConnectToMac(const Capabilities& caps) { return caps.usbSetup && offersNetworkProfile(caps); }
// Check connection ends with the mailbox, so it is offered where the mailbox is.
constexpr bool offersConnectionCheck(const Capabilities& caps) { return caps.mailbox; }
// "Reset the clock check" (Settings > System > Clock): the recovery for a clock floor that is wrong. The floor and the
// clock record exist wherever the mailbox's HTTPS or a certificate network needs a trusted time, so the reset is
// offered there whether or not the RTC chip answered its probe: a reader whose chip failed to probe must not need a
// flash to clear a bad floor.
constexpr bool offersClockReset(const Capabilities& caps) { return caps.mailbox || offersNetworkProfile(caps); }

// ---- the device ------------------------------------------------------------------------------------------------

// The board this firmware runs on, from the SDK's active profile. Cheap (a few comparisons) and stateless: call it
// where it is needed. On the C3 boards the profile is final once HalGPIO::begin() has run; they are unsupported
// either way.
Board board();

}  // namespace plaque
