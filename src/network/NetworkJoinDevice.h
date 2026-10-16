#pragma once

#include <cstdint>

#include "NetworkJoin.h"

// Joining the installed network profile on the reader (NetworkJoinDevice.cpp). The order of candidates is the pure
// netjoin logic (NetworkJoin.h); this is the radio side: open, PSK and EAP-TLS joins, and the EAP state.
//
// EAP-TLS: the supplicant keeps pointers to the CA chain, the client certificate and the key rather than copies, so
// those buffers live here, in PEM as the ESP-IDF enterprise example passes them, for as long as the station may
// (re)authenticate with them. releaseEnterprise() disables enterprise mode, clears every EAP setting and only then
// zeroes and frees the buffers; ambient::disconnectWifi() calls it before the radio goes off, and so does every
// failed or abandoned profile join, so a later open or PSK join starts clean. Main loop task only.
namespace netjoin {

enum class Poll : uint8_t { Pending, Connected, Failed };

// How long a screen that joins the profile while someone watches (the Messages and Register checks) waits for it before
// it offers the saved networks instead: as long as the connection check gives it (SPEC section 8).
constexpr uint32_t INTERACTIVE_PROFILE_JOIN_MS = 20000;
constexpr uint32_t INTERACTIVE_PROFILE_EAP_JOIN_MS = 30000;
constexpr uint32_t interactiveProfileJoinMs(const bool eapTls) {
  return eapTls ? INTERACTIVE_PROFILE_EAP_JOIN_MS : INTERACTIVE_PROFILE_JOIN_MS;
}

// Starts joining the installed profile without waiting (the radio settles first, as for a saved network). False when
// there is no profile or it cannot be read; that reads as a failed join.
bool beginProfileJoin();
// Connected only once the network is joined with the authentication the profile asks for (an EAP-TLS profile never
// accepts an open network that happens to share its name).
Poll pollProfileJoin();
// Gives up on a profile join in progress: disconnects and releases the EAP state.
void abortProfileJoin();
// Disables enterprise mode, clears the EAP settings and wipes their buffers. Idempotent; call before the radio goes
// off.
void releaseEnterprise();

// A timer wake's join: the profile, then the last-used saved network, together within the join's share of the wake.
// Blocking. True when connected. The sleep-event delivery before deep sleep (main.cpp) joins the same way.
bool joinForWake();
// Whether joinForWake() has anything to try: an installed profile, or a saved network that was joined last.
bool hasWakeCandidate();
// The longest joinForWake() can take with the profile that is installed now (profile and last-used network: each has
// its own budget, see NetworkJoin.h); 10 s with no profile, as it always was. Needs only the profile (NVS), not the
// card, so main.cpp can ask it before the card is mounted.
uint32_t wakeJoinWorstMsNow();
// Who joined last, kept in RTC memory across deep sleep (a hint for the order of the next wake's join). A profile join
// records itself; a saved network is recorded by the wake join and by the Method shelf's check.
Joined lastJoined();
void rememberJoined(Joined joined);

// Whether the clock counts as trusted for EAP certificate dates: a fresh SNTP sync or a trusted retained clock (SPEC
// section 7).
bool eapClockTrusted();
// Whether the EAP server certificate's dates are actually checked on this join. The supplicant leaves dates to
// mbedTLS, which judges them only when built with MBEDTLS_HAVE_TIME_DATE; this firmware's ESP-IDF build is not, so
// this is false whatever the clock says (the CA and the server name are still checked).
bool eapServerDatesChecked();
// The driver's reason for the station's last disconnect (wifi_err_reason_t), 0 when none was seen.
uint8_t lastDisconnectReason();
// True when the network up now was joined through the profile.
bool joinedViaProfile();

}  // namespace netjoin

// A timer wake's join and the end of every Wi-Fi session, as the ambient code names them (also declared in
// ambient/LatestMessageProvider.h): connectSavedWifi() is joinForWake(); disconnectWifi() leaves the network, clears a
// profile's EAP state and switches the radio off.
namespace ambient {
bool connectSavedWifi();
void disconnectWifi();
}  // namespace ambient
