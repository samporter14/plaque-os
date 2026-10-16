#pragma once
// The fake reader the glue tests run the real device code against: a Wi-Fi radio with access points in range, the
// supplicant's EAP state, NVS, the saved-network store, the clock, the log. Everything is in one struct so a test
// can reset it and read what the code did.
#include <PlaqueBoard.h>
#include <esp_eap_client.h>
#include <esp_wifi.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "WiFi.h"

namespace fake {

// One access point in range. The radio decides whether a station config can join it the way the real network would:
// an open one takes anybody, a PSK one the right passphrase, an enterprise one an EAP-TLS config that is complete (and
// whose RADIUS server accepts it).
struct AccessPoint {
  std::string ssid;
  wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
  std::string psk;
  int rssi = -50;
  int channel = 6;
  uint32_t joinMs = 1500;     // from connect to associated and authenticated
  bool hidden = false;        // does not announce itself to a scan
  bool radiusAccepts = true;  // enterprise: the server takes this reader's certificate
  std::string radiusDomain;   // enterprise: the name its certificate carries ("" = whatever is configured)
};

// What the supplicant was told (esp_eap_client_*), and when it was torn down.
struct Eap {
  bool enterpriseEnabled = false;
  uint32_t methods = ESP_EAP_TYPE_ALL;  // the IDF default: all of them
  bool useDefaultBundle = true;
  bool identitySet = false;
  std::string identity;
  bool caSet = false;
  bool certSet = false;
  bool keySet = false;
  std::string domain;
  bool disableTimeCheck = false;
  int enableCalls = 0;
  int disableCalls = 0;
  int clearCalls = 0;
};

struct Radio {
  // Access points in range, and the station.
  std::vector<AccessPoint> aps;
  wifi_mode_t mode = WIFI_MODE_NULL;
  wifi_config_t config = {};
  wifi_auth_mode_t minSecurity = WIFI_AUTH_WPA2_PSK;
  enum class State : uint8_t { Idle, Connecting, Connected, Failed } state = State::Idle;
  uint32_t connectStartedAt = 0;
  int target = -1;         // index in aps of the access point the attempt is going to
  uint32_t settledAt = 0;  // virtual ms at which the attempt ends
  bool outcomeIsFailure = false;
  int failReason = 0;
  wl_status_t failStatus = WL_CONNECT_FAILED;
  wifi_auth_mode_t joinedAuth = WIFI_AUTH_OPEN;
  std::string joinedSsid;
  // What happened.
  struct Begin {
    std::string ssid;
    bool hadPassphrase = false;
    std::string passphrase;
    bool connectNow = true;
  };
  std::vector<Begin> begins;
  // Every esp_wifi_connect(): the auth threshold the config carried and the access point it was going to join (-1:
  // none was acceptable), so a test can see that an open or PSK twin was never a candidate.
  struct Attempt {
    std::string ssid;
    wifi_auth_mode_t threshold = WIFI_AUTH_OPEN;
    int target = -1;
    bool enterpriseEnabled = false;
  };
  std::vector<Attempt> attempts;
  int disconnects = 0;
  int modeOffs = 0;
  int connectCalls = 0;
  // A scan.
  bool scanning = false;
  uint32_t scanDoneAt = 0;
  std::vector<AccessPoint> scanResult;
  // Events: handlers registered through WiFi.onEvent.
  struct Handler {
    NetworkEventSysCb callback;
    arduino_event_id_t filter;
    size_t handle;
  };
  std::vector<Handler> handlers;
  size_t nextHandle = 1;
  // Addressing once joined.
  bool leaseGiven = true;
};

// The saved networks and which was joined last (WifiCredentialStore).
struct SavedNetwork {
  std::string ssid;
  std::string password;
};
struct Store {
  std::vector<SavedNetwork> networks;
  std::string lastConnected;
  int outOfRangeReads = 0;  // getCredentialAt/getSsidAt asked for an index that is not there
  int loads = 0;
};

// The ESP-IDF NVS, in memory: namespace -> key -> bytes.
struct Nvs {
  std::map<std::string, std::map<std::string, std::vector<uint8_t>>> data;
  size_t availableEntries = 400;
};

// The system clock, the SNTP client and the RTC timer behind lib/TrustedTime and message_tls. TrustedTime itself is the
// real one; this is what it talks to.
struct Clock {
  // SNTP, as lwIP's client answers it: a sync completes ntpDelayMs of virtual time after configTzTime() (re)starts the
  // client, setting the system clock to ntpEpoch, or never when ntpAnswers is false. A completion is reported to
  // sntp_get_sync_status() once (the read consumes it) and to the registered notification callback.
  bool ntpAnswers = true;
  int64_t ntpEpoch = 0;
  uint32_t ntpDelayMs = 0;
  bool sntpEnabled = false;
  bool sntpFired = false;
  bool sntpStatusPending = false;
  uint32_t sntpStartedAt = 0;
  int configTzCalls = 0;
  int sntpStops = 0;
  // lwIP's three server slots: the name a slot was given (empty: none) and the address a DHCP lease put in it.
  char slotName[3][64] = {};
  bool slotNamed[3] = {false, false, false};
  unsigned slotAddress[3] = {0, 0, 0};
  bool dhcpMode = false;  // esp_sntp_servermode_dhcp(): a lease's NTP server is taken
  wifi_ps_type_t powerSave = WIFI_PS_MIN_MODEM;
  // The system clock (what time() and gettimeofday() answer).
  int64_t timeEpoch = 0;
  uint64_t rtcMicros = 0;                            // esp_rtc_get_time_us()
  int resetReason = 1;                               // esp_reset_reason(): 1 power on, 3 deep sleep
  std::vector<std::vector<std::string>> serversSet;  // every configTzTime() call, as the list of names it was given
  std::vector<bool> dhcpFlagSet;                     // every esp_sntp_servermode_dhcp() call
  // The order in which clock and radio things happened, for "before the join" assertions.
  std::vector<std::string> trail;
};

// The BM8563 RTC chip (HalClock).
struct Chip {
  bool readable = false;  // answers with its VL flag clear
  int64_t epoch = 0;      // what it read when it was last set; it then counts on the RTC timeline (Clock::rtcMicros),
  uint64_t setAtRtcMicros = 0;  // not the system clock's, which a sync can step
  bool writable = true;
  std::vector<int64_t> written;
};

struct Card {
  bool ready = true;
  std::map<std::string, std::string> files;
};

// The board the firmware runs on, as the SDK's profile reports it (plaque::board()): an X4 Pro unless a test says
// otherwise.
inline plaque::BoardFacts proBoardFacts() {
  plaque::BoardFacts facts;
  facts.x4Pro = true;
  facts.touch = true;
  facts.homeKey = true;
  facts.frontlight = true;
  facts.usbDrive = true;
  return facts;
}

struct World {
  plaque::BoardFacts boardFacts = proBoardFacts();
  uint32_t now = 1000;  // virtual millis()
  Radio radio;
  Eap eap;
  Store store;
  Nvs nvs;
  Clock clock;
  Chip chip;
  Card card;
  std::vector<std::string> log;  // every log line, "E tag: text" for errors
  int errorLogs = 0;
  int hostnameApplied = 0;
  bool renderLockBusy = false;  // RenderLock::Mode::Try finds the lock taken
  int renderLockTaken = 0;
};

World& world();
void reset();  // a fresh world

// Moves the virtual time on, letting the radio and any scan settle on the way.
void advance(uint32_t ms);
// Counts log lines containing `text`.
int logCount(const std::string& text);
// A DHCP lease arrives carrying an NTP server: lwIP keeps it in slot 0 if the DHCP option (esp_sntp_servermode_dhcp)
// was on when the lease was taken, and ignores it otherwise.
void deliverLeaseNtpServer(unsigned address);
// The SNTP client's completion check; advance() runs it.
void sntpStep();

}  // namespace fake
