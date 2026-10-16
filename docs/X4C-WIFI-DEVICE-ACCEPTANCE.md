# X4 Classic and university Wi-Fi: device acceptance

Nothing below has been run on a reader. Host tests and builds do not prove hardware behaviour.
Do the **X4 Classic only** items before trusting a Classic; do the **X4 Pro regression** items on
the Pro before and after flashing, because the shared code changed.

Flash writes, SD card writes and enrollment are done by the person at the bench; no automated test does them.

## 0. Before flashing a Classic

1. **Read the Classic's real partition table** (read-only) and compare it with `partitions-x4c.csv`
   (nvs 0x9000/0x5000, otadata 0xE000/0x2000, app0 0x10000/0x7E0000, app1 0x7F0000/0x7E0000,
   spiffs 0xFD0000/0x14000, coredump 0xFE4000/0x1C000). That file comes from the FreeInk SDK
   document, not from a Classic.
   `esptool.py --port <port> read_flash 0x8000 0x1000 ptable.bin`
   If it differs, stop: do not flash. The installed image must fit the Classic's app slots.
2. Keep a backup of the Classic's stock flash before the first write.
3. Prefer the SD card installer (Settings, Install from card) over a USB write; it only writes
   the app slot, and refuses an image whose board tag names the other board.

## 0b. Things the SDK itself lists as pending hardware for the Classic, and one figure that is an assumption

| # | Do | Expect / record |
|---|---|---|
| 1 | Boot the Classic and look at Home, a Method page and Settings | **Panel orientation and flip** (`NO_FLIP`, "panel mount transform pending hardware"): the page is upright and not mirrored. If it is not, record how (mirrored, rotated, upside down): the profile's flip needs setting. |
| 2 | Look at the edges: the scrollbar, the key hints, the first and last lines of text | **Bezel insets** (top and bottom are the X4's historical values, "pending measurement"): nothing is hidden by the bezel or crowds it. Record the millimetres lost, if any. |
| 3 | Charge the reader on USB and watch the battery indicator, and unplug it | **Charge status (STAT, GPIO21)**: it shows charging while charging and not otherwise. If it is inverted the polarity assumed (active-high) is wrong. |
| 4 | Press nothing, then watch GPIO4 in the serial log if the SDK logs it | **GPIO4** is configured as an input and not interrupt-attached; record what it reads, and whether anything on the case is wired to it. |
| 5 | **Measure the internal clock's drift.** The retained-clock bound assumes the ESP32-S3's internal slow clock drifts by 2 % (20 000 ppm); that figure is an assumption, not a measurement. Run Check connection (it records an SNTP sync), put the reader into deep sleep for one hour and, on another run, for eight, with a timer wake and no network in range, then read `/Lamina/connection-check.json` after waking on a network that blocks NTP (`clock: "retained"`, `finished_at`) or the `CLOCK: Retained clock` log line, and compare the reader's time with real time | Record the error against the sleep time, in ppm. If it is well under 20 000 ppm the bound is safe but wastes the retained clock's reach (about 8.3 hours at 2 %); if it is over, `clockprov::INTERNAL_SLOW_PPM` must go up. Record the temperature. |
| 6 | **Does the system clock run fast through a sleep, and does the chip stay right?** After the one-hour and eight-hour sleeps of item 5, and a 24-hour one, read the chip's time against real time through the `CLOCK: Retained clock` log line (`RTC chip`, age, bound) | The chip is within a few seconds of real time and the line says `trusted (RTC chip …)` however far the internal clock ran ahead: the proven floor does not include the internal clock. Record the chip's error and the internal clock's. |

## 1. Each board: basic bring-up (X4 Classic and X4 Pro)

| # | Do | Expect |
|---|---|---|
| 1 | Cold boot (power off, then on) | Plaque mark, Home appears, Method present. Serial log shows `ESP32S3_X4_CLA` (Classic) or the Pro tag. |
| 2 | Home with keys only (Classic) / keys and touch (Pro) | Every Home item reachable. Classic shows no touch or Home-key hints. |
| 3 | Settings › About | Model row, **Wi-Fi MAC** with "The address to register for Wi-Fi", UUID, firmware. Classic shows no Touch or Frontlight rows. |
| 4 | Remove the card, boot, insert it, boot again | `/Lamina/reader.json` exists at every boot, schema 2, with `model`, `station_mac`, `hotspot_mac`, `capabilities`. Read it on the Mac with no network. |
| 5 | Settings › System › Network | Station MAC, "2.4 GHz only", profile line, Check connection, Connect to Mac, Import from card. Back returns. |
| 6 | Method shelf | Opens with no network wait. Open a book, page with side keys, Confirm opens Steps. |

## 2. Each board: keys-only walk (Classic must pass with no touch and no Home key)

Method shelf select and open; Remove (long-press Confirm, choice is not where the finger was);
page turns; Steps menu; start a timer with Confirm ("Start 5:00 timer"); stop it; Messages
settings; Network screen and every row in it; Connect to Mac; Check connection and cancel
with Back; Settings › System › Network › Remove profile (asks first). Pro: repeat by touch too.

## 3. Sleep, resume, timer wake, alerts (both boards)

| # | Do | Expect |
|---|---|---|
| 1 | Open a Method step, wait for auto-sleep, press power | The step stays on the panel while asleep (moon mark), wake returns to that page. |
| 2 | Same, with scheduled polling on and a new note waiting; let a timer wake happen | The note is fetched; the step stays on the panel (not redrawn). Button wake returns to the page. **Classic: the panel image surviving a timer wake is unverified.** If it blanks, report it. |
| 3 | Start a 1 minute timer, sleep | At zero: wake, timer box inverts, reader stays awake until a key press. **Pro:** frontlight pulses. **Classic:** no light, no sound, no vibration, and no wording that promises any. |
| 4 | Press a key at the alert | Alert clears, same page, timer marked done. |
| 5 | Let a timer alert come due during Check connection | The check cancels cleanly and the alert takes over. |
| 6 | Power loss with a timer running (pull battery if safe) | "Power was lost · the timer may be over". |

## 4. Scheduled downloads and radio cleanup (both boards)

| # | Do | Expect |
|---|---|---|
| 1 | Polling on, send a protocol to this reader (Mac Send to Reader) | Next timer wake downloads it within the battery/time budget; shelf shows it NEW. |
| 2 | Cancel a shelf check with Back mid-download | Partial file gone, Wi-Fi off, heap freed before the reader opens. |
| 3 | After every check and wake, watch current draw or the Wi-Fi LED/log | Radio off: log shows Wi-Fi disconnect, `WiFi.getMode()` null. |
| 4 | Battery under 15 % | No fetch; message "Battery low". |
| 5 | Two readers on one mailbox | Targeted send reaches only the named reader. |

## 5. USB and card setup (both boards)

| # | Do | Expect |
|---|---|---|
| 1 | Plug in USB, open **Connect to Mac**, run Plaque › Settings › Readers › Set Up University Wi-Fi… | Mac picks the reader's port itself (the one Espressif USB device; any other port is chosen by hand), shows model, MAC, UUID before any cloud check-in. **USB serial on the Classic is unverified.** |
| 2 | Send an open profile over USB | Reader shows name, SSID, auth; **Install** and **Back** are separate large choices; Back declines. After Install: `/Lamina/network-profile.json` updates with no secret in it. |
| 3 | Send a synthetic EAP-TLS profile over USB | Same; server name and client certificate subject/expiry shown. |
| 4 | Send a bad profile (mismatched key) | Mac shows the plain reason; reader keeps the old profile. |
| 5 | Replace an installed profile, unplug USB mid-transfer | Old profile still works. |
| 6 | Put an open/PSK bundle in `/Lamina/Provision/` with USB Drive or a card reader; choose **Import from card** | The install question appears (row 11); choose **Install**: installed; the inbox file is gone from the moment it was read; `/Lamina/network-import.json` has no secret. |
| 7 | Put an EAP-TLS bundle with a key in the inbox | Refused at once (`card_key_refused`), with no question; file zeroed and deleted. |
| 8 | USB Drive mode (File Transfer) | Reader writes nothing to the card while the computer owns it; boot-time `reader.json` is skipped then. **USB Drive on the Classic is unverified.** |
| 9 | Remove profile | Asks; after Remove, saved networks still work. |
| 9a | With the reader's Install screen showing, press Stop (or Esc, or close the Mac window) | The Mac stops within a second and says "Stopped. The reader may still be showing its question: choose Back on it…". The reader's screen is unchanged and Back still declines; Install there before the question times out (9e) would still install the profile, which is the person's choice at the reader. |
| 9a2 | Then send again from the Mac while that question is still up | The Mac says "The reader is still asking about an earlier profile. Choose Back on it, then try again." (not "The reader didn't answer"); the reader answers `ERR busy`. After Back on the reader, sending works. |
| 9b | Let the Install (or Keep/Remove) screen sit past the Mac's wait | The Mac says it was waiting for your choice on the reader and that the reader may still be asking, not that the reader stopped answering, and does not claim nothing was installed. After 2 minutes in all the reader takes the question as Back (9e). |
| 9e | Open the Install question (send an open profile over USB) and touch nothing for 2 minutes, with the Mac still connected; then do it again and touch a choice's highlight (Up/Down) every minute for 4 minutes | The first: at 2 minutes the dialog goes by itself and the screen says "Nobody answered, so nothing was changed. Send it again from the Mac."; the Mac gets `ERR declined`; nothing is installed and sending again works. The second: the question stays while it is worked (each change of the highlight) and goes 2 minutes after the last one. Same for the Remove question. |
| 9f | Open the Install question, then unplug the USB cable (or turn the Mac off) and wait | After a few seconds the dialog goes and the screen says "The Mac is no longer connected, so the question was dropped and nothing was changed." Plug in and send again: it works. Unplug in the middle of a transfer: the transfer is dropped and the screen is back to listening. |
| 9c | Open the reader's port in a serial logger, then send from the Mac | The Mac says another program is using the port and does not send. Close the logger and it works. |
| 9d | Write a passphrase profile "On the card" for a card from older firmware (no schema 2 `reader.json`) | Refused with a sentence pointing to USB; nothing is on the card. |
| 10 | Open and leave **Connect to Mac** ten times, with and without the Mac connected | No crash, watchdog reset or lost serial log. The reader enlarges the USB receive queue while the screen is open and restores it on exit. Log shows no "Could not enlarge/restore the USB receive queue". |
| 11 | Put an open or PSK bundle in `/Lamina/Provision/` and choose **Import from card** on a reader that already has a profile | A question appears (the file is already gone): network name, security, "The password is stored unencrypted in the reader." for a PSK, and "Replaces <name>, <kind>." (with "Its key is deleted from the reader." if the installed one is a certificate profile). **Back is highlighted.** Back (or leaving the screen) installs nothing and `/Lamina/network-import.json` says `rejected` / `declined`; Install replaces it and says `installed`. While the question is up the file says `pending` (read it from a computer with the reader switched off at that moment: it still says `pending`, never an earlier `installed`). |
| 12 | Send a synthetic EAP-TLS profile over USB with the Mac's normal unpaced sender while the screen is busy (just opened, mid-refresh) | The question says the server certificate's dates are not checked and that the key is stored unencrypted. No `ERR checksum`; the log shows no "USB receive queue of N bytes, not 24576" line. |

## 5b. The clock record and "Reset the clock check" (both boards)

| # | Do | Expect |
|---|---|---|
| 1 | Settings › System › Clock › **Sync clock now** on a network with a profile that names its own `time_servers` | The sync asks the profile's servers (not `pool.ntp.org`); on success the chip is set (the displayed time is right). The RTC is written only through the clock record: the log says `Sync recorded: …`. |
| 2 | Do 1 again an hour later | `Sync recorded: the RTC chip agrees`; the record is confirmed. |
| 3 | With a test NTP server you control (or a network that serves a wrong time), make the next sync answer a day, then 90 days ahead | The sync is used for this session and the log says `used for this session only: the RTC chip disagrees`; the displayed (chip) time is unchanged; after a reboot, HTTPS (Check connection) works on a good network. |
| 4 | Make a sync answer a year in the past | `not recorded: behind the clock floor`; the chip is unchanged. |
| 5 | Make a sync answer a time past 2100 | Not a sync (`HTTPS skipped`/`Couldn't synchronize the clock.`); nothing recorded. |
| 6 | On a reader whose chip was just replaced or lost its battery, sync | `Sync recorded: the RTC chip cannot vouch`; the floor in NVS moves by a minute at most (no tool reads it on the reader; the next HTTPS works). |
| 7 | Settings › System › Clock › **Reset the clock check** | A question appears with "Use this if the reader shows a date in the future and can't connect."; **Keep** is highlighted (a finger that opened it has Keep under it). Keys: Up/Down switch, Confirm chooses, Back keeps. Reset: the row says "Done. The clock is set again the next time the reader connects."; the log has `The clock check was reset`. Reachable with the Pro's side buttons, Power and the Home pad and by touch, and on the Classic with its keys only. |
| 7b | After 7 (or on a reader whose RTC probe failed, so the Clock row exists only through `offersClockReset`) | The reader reads as having no time (the status-bar clock and Register say so) until the next connection; HTTPS does not use the 90 seconds a sync gave before the reset. |
| 7c | Replace the earlier firmware (2a6befc) with this one after the reader has slept long on it, then connect | The first sync is recorded although the floor the old firmware left was ahead of true time (log: `lowered to`); Sync clock now says synced, not "Sync failed". |
| 8 | After 7, run Check connection | A fresh first sync; `clock: "sntp"`; and an hour later the next sync is confirmed. |
| 9 | After a failed **Sync clock now** | The screen says "If the date is wrong, use Reset the clock check." |

## 6. Check connection (both boards)

Run it on each network below and read `/Lamina/connection-check.json` on the Mac.

| Case | Expect |
|---|---|
| Healthy network | All six stages ok; "Connected to the mailbox." `finished_at` set after a time sync. |
| Wrong network password (saved network) | `join` failed: "Couldn't join this network." |
| Network with no internet (DNS blocked) | `dns` failed: "Wi-Fi joined. Couldn't resolve the mailbox." |
| UDP 123 blocked (needs IT help or a test network) | `time` failed unless a trusted retained clock exists: "Couldn't synchronize the clock." |
| Wrong token (change the token in a test config) | `mailbox` unauthorized: "Mailbox access was rejected." |
| Back pressed in each stage | that stage `canceled`, later stages `not_run`, Wi-Fi off, result saved. |
| Clock unset, check ends | `finished_at` is null; the Mac says "time unknown". |

Never test with the production token in a shared screenshot or log.

## 6b. Joining (both boards)

| # | Do | Expect |
|---|---|---|
| 1 | With a profile and a saved network, run Check connection, then the Method shelf's check, on the profile's network | Both join the profile (Check connection did not before this fix). |
| 2 | Join the saved network (home) once through the shelf, sleep, let a timer wake fetch | The wake tries home first, then the profile only if home fails. Back on campus, join the profile once: the next wake tries the profile first. |
| 3 | EAP-TLS profile, wake in range of campus | The wake's join has up to 25 s for the profile; a slow handshake that used to be cut off at 10 s completes. |
| 4 | EAP-TLS profile with a test AP that broadcasts the same name as an **open** network and is closer | The reader joins the enterprise network, or fails; it never associates with the open one. |
| 5 | A plugin that subscribes to sleep.enter, a profile and no saved network | Sleeping delivers over the profile's network; with a certificate profile the supplicant state is cleared and Wi-Fi is off afterwards. |
| 6 | File Transfer, OPDS, KOReader Sync or the clock-sync screen on a profile-only network | They cannot join it (they use the saved networks only). That is the documented limit; record that it is so. |
| 7 | A network that blocks UDP 123, with a sync recorded in the last few minutes | The Method shelf's check proceeds on the retained clock, as Check connection does ("Time kept by the reader since its last sync."). |
| 8 | A profile with `use_dhcp_time` on a network whose DHCP offers an NTP server the profile's `time_servers` do not name | The lease's server is asked first on the first sync after the join. |
| 9 | Settings on a reader with no profile | No error line about a missing profile in the log at any check. |

## 7. U-M campus (ITS) network acceptance

External requirements: a person with a U-M account, a **personal** (not university-managed)
device, and ITS' registration site. Registered Plaque readers on MSetup are subject to ITS
policy; this software does not claim ITS approval.

1. Mac › Set Up University Wi-Fi… › U-M campus — ITS. Confirm: station MAC (not the hotspot MAC)
   is shown and copyable; the 2.4 GHz note is visible; the MSetup page opens in the browser.
2. In the browser, register the station MAC under "Other Device" (you sign in there).
3. Send an open profile named for MSetup (SSID `MSetup`, editable) over USB.
4. Run Check connection in an Ann Arbor or Dearborn campus building outside Michigan Medicine
   areas. Record each stage and the Wi-Fi band the reader associated on (2.4 GHz expected).
5. Read the Security note in the app: MSetup/MGuest are not for sensitive institutional data. If
   protocols, notes or tables carry such data, use an institution-approved secure profile.
6. MGuest: confirm the app says it is unsupported (a captive portal needs a sign-in the reader
   cannot do) and that no password field exists anywhere on the reader.
7. University-owned or managed devices: do not self-register; contact unit IT or the ITS Service
   Center (734-764-HELP).
8. Check scheduled polling: wake, fetch, radio off, panel unchanged.

## 8. Michigan Medicine (HITS) network acceptance

External requirements: HITS must agree the device may connect, tell you the network and
authentication it will accept, and issue any profile or certificate. MSetup's IoT instructions do
**not** apply in Michigan Medicine areas, and the app pre-fills no SSID.
HITS guidance: KB0018780 "How to have a device Managed by Michigan Medicine" (sign-in required);
HITS Service Desk 734-936-8000.

1. Mac › Set Up University Wi-Fi… › Michigan Medicine — HITS.
2. **Export the reader information** (review it, add building/room and contact, Copy or Save). It is
   never sent for you. Give it to HITS.
3. Enter the network name, authentication and files HITS supplies (open/registered, a per-device
   passphrase, or EAP-TLS identity, server name, CA certificate, client certificate and key).
   EAP-TLS goes over USB only.
4. Send to the reader; confirm on its screen.
5. Run Check connection on the reader inside the HITS-covered area and record every stage.
6. Give HITS the check result if they ask; if a stage fails, report the observed fact, not a guess.
7. If HITS cannot support a device on its network, stop. Do not fall back to a password login.

## 9. What remains outside this software

- HITS approval, a HITS network name and EAP-TLS credentials, and any institutional registration.
- Real IT-issued certificates. All tests used generated synthetic ones.
- Real-card provisioning and flashing.
- The Classic's real partition table (section 0) and Classic USB serial and USB Drive behaviour.
- NVS and flash encryption are **off** and nothing here turns them on (that burns eFuses
  irreversibly). A profile's keys are in the reader's internal flash, not on the card, but anyone with
  physical access and a USB cable can read that flash. Removing or replacing a profile *deletes* its
  entries (`nvs_erase_key`, which is logical): the bytes stay in the flash page until NVS reclaims it,
  so a removed key is not scrubbed from the chip. Use a per-device certificate IT can revoke.
- The supplicant does not judge the EAP server certificate's dates on this build; the CA chain
  and server name are still required.
