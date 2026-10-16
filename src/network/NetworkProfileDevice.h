#pragma once

#include <string>
#include <vector>

#include "NetworkCardImport.h"
#include "NetworkProfile.h"

// The reader's installed network profile: the NVS store, the card files and the accessors the rest of the firmware
// uses (NetworkProfileDevice.cpp). The decisions are in the pure code (NetworkProfile, NetworkProfileStore,
// NetworkCardImport); these are thin adapters. Main loop task, unless a function says otherwise.
namespace netprofile {

// The installed profile's public part, read from NVS once and kept (safe from any task). installed is false when
// there is none, or when the stored one does not check out.
Summary summary();
bool installed();
// Reads the installed profile again from NVS. install(), remove() and the card import do it themselves; this is for
// the case where the profile was changed behind the cache's back (and for the host tests, which stage NVS directly).
void reload();

// For the clock (SPEC section 7; safe from any task): the profile's NTP servers (empty: the reader's defaults, also
// when no profile is installed) and whether DHCP-offered NTP servers are accepted too.
std::vector<std::string> activeTimeServers();
bool activeUseDhcpTime();
// Gives the clock the installed profile's time servers and DHCP option, or its defaults with no profile. Every sync
// calls it through message_tls::prepareClock() / beginClockSync() (the clock keeps the servers a running sync began
// with), so no caller of a sync needs to.
void applyTimeSettings();
// Only the DHCP option: lwIP takes an NTP server from a lease as the lease arrives, so the option has to be on before
// the join that gets the lease (TrustedTime.h). The joins call it before they start; with no profile it turns the
// option off, which is where it starts.
void applyDhcpTimeSetting();

// The whole installed profile, secrets included, for a join. False when there is none or it cannot be read.
bool loadForJoin(Profile& out);

// Validates a bundle with the reader's clock (dates judged only with trusted time) and random source.
Error validate(std::string_view text, Source source, Profile& out);
// Stores a validated profile (all or nothing) and refreshes the card's summary file.
Error install(const Profile& profile);
// Erases the installed profile and refreshes the card's summary file. False when it could not be removed.
bool remove();

// /Lamina/network-profile.json, rewritten only when it differs from what NVS holds. Called once at boot and after
// every change; does nothing while the card is unavailable.
void refreshSummaryFile();
// What that file says, from NVS (the PROFILE reply).
std::string summaryText();

// Import from card, in two steps (NetworkCardImport.h) because the person is asked in between. Respects USB Drive:
// nothing is touched while the computer owns the card. beginCardImport() reads the inbox, validates it and wipes it; a
// pending read holds the profile, and the caller shows what it is and what it replaces. finishCardImport() installs it
// (or declines), writes /Lamina/network-import.json and refreshes the summary file.
CardRead beginCardImport();
CardResult finishCardImport(CardRead& read, bool install);

// Trusted unix time when the clock is trusted for certificate dates, else 0.
int64_t trustedTime();

}  // namespace netprofile
