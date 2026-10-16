# Plaque on the X4 Classic

The Xteink X4 Classic (X4C, env `x4c`) is the X4 Pro's ESP32-S3 board and 800×480 glass with
buttons only: no touchscreen, no capacitive Home key, no frontlight. Plaque runs on it as on the
X4 Pro. Its features are asked for by capability, not by board: a small capability layer
(`lib/PlaqueBoard`) says what the board *is* (the model) and what Plaque *offers* on it. The
contract this follows is `SPEC-UNIVERSITY-WIFI.md` sections 1 and 2 (in the Mac repository's
`docs/`).

## The capability layer

`lib/PlaqueBoard/PlaqueBoard.h`:

- `plaque::Model`: `X4Pro`, `X4Classic`, `Unsupported` (every other board, C3 X4/X3 included).
- `plaque::Capabilities`: hardware bits (`keys`, `touch`, `homeKey`, `frontlight`) and Plaque
  features (`method`, `mailbox`, `notes`, `almanac`, `scheduledDownloads`, `timedWake`,
  `stepTimers`, `alertLight`, `cardIdentity`, `usbDrive`, `usbSetup`, `wifiOpen`, `wifiPsk`,
  `wifiEapTls`).
- `plaque::board()` (device side, `PlaqueBoardDevice.cpp`) derives the model from the SDK's
  active profile (`BoardConfig::isX4Pro()`, `BoardConfig::isX4Classic()`) and narrows each
  hardware bit to what the profile reports *and* what the build compiles in
  (`FREEINK_CAP_TOUCH && BoardConfig::hasTouch()`, `BoardConfig::hasHomeKey()`,
  `FREEINK_CAP_FRONTLIGHT && hasPwmFrontlight()/hasI2cFrontlight()`, `FREEINK_CAP_USB_MSC`).
  A build without a driver never claims the hardware (fail safe). It is stateless and cheap.
- Pure (host-tested in `test/plaque_board`): `capabilitiesFor(model)`, `boardFor(facts)`,
  `modelId()` (`x4pro`, `x4c`), `modelName()` (`Xteink X4 Pro`, `Xteink X4 Classic`),
  `capabilityTokens()` (the reader.json list), and the decisions screens make:
  `promptsTouch()` (a prompt names a tap or a key), `ambientSleepScreen()`,
  `armedWakeMicros()` (no timer is armed on a board without timed wake), `alertHasLight()`, `offersRegister()` (the
  Register sleep screen and its Messages rows).

| | X4 Pro (`x4pro`) | X4 Classic (`x4c`) | any other board |
|---|---|---|---|
| keys | yes (side pair, power, Home key) | yes (side pair, power, Left/Right/Confirm/Back) | profile's |
| touch, Home key | yes | **no** | profile's |
| frontlight | yes | **no** | profile's |
| Method, mailbox, Notes, Almanac, Register | yes | yes | no |
| scheduled downloads, timed wake, step timers | yes | yes | no |
| step-timer alert light (`alertLight`) | yes | **no** (inverted box only) | no |
| `/Lamina/reader.json` (`cardIdentity`) | yes | yes | no |
| USB Drive | yes (`FREEINK_CAP_USB_MSC`) | yes (`FREEINK_CAP_USB_MSC`) | build's |
| network tokens (`usb_setup`, `wifi_open`, `wifi_psk`, `wifi_eap_tls`) | yes | yes | no |

Tokens in reader.json, in this order: `keys`, `touch`†, `frontlight`†, `method`, `notes`,
`almanac`, `step_timers`, `timed_wake`, `scheduled_downloads`, `usb_drive`, `usb_setup`,
`wifi_open`, `wifi_psk`, `wifi_eap_tls`, `register` († X4 Pro only). The Classic's list is the
contract's example followed by `register`, which is additive: it comes last, so every earlier token keeps its name and
place (`SPEC-UNIVERSITY-WIFI.md` section 2).

## What each capability gates

Every bit of `plaque::Capabilities` either gates something real, in the files named here, or is **advertised only**:
it is a `reader.json` token and nothing in the firmware asks for it. `test/test_x4c_capability_docs.py` keeps this table
true: it checks that each row's helper is defined in `lib/PlaqueBoard/PlaqueBoard.h` and mentions the bit, and that
each file named as a consumer uses the helper (or the bit itself), and that no bit of the struct is missing here.
A board Plaque does not run on has every bit off except its own hardware, so none of the Plaque features below exist on it.

<!-- capability-gates -->
| bit | gates | helper | consumers |
|---|---|---|---|
| `keys` | advertised only: every supported board has the keys Plaque needs; nothing asks | - | - |
| `touch` | tap or key wording in prompts (the step timer, the alert, the Register full-day footer); About's Touch row | `promptsTouch` | `src/activities/reader/StepTimerView.cpp`, `src/activities/reader/StepTimerAlertActivity.cpp`, `src/activities/settings/AboutActivity.cpp`, `src/ambient/RegisterProvider.cpp` |
| `homeKey` | advertised only: screens ask the SDK (`gpio.hasHomeKey()`); it is not a `reader.json` token | - | - |
| `frontlight` | About's Frontlight row; `alertLight` follows it | - | `src/activities/settings/AboutActivity.cpp` |
| `method` | Method on Home; the Method sleep that keeps a step on the panel | - | `src/activities/home/HomeActivity.cpp`, `src/main.cpp` |
| `mailbox` | Settings > Messages; the mailbox settings at boot; Check connection | `offersConnectionCheck` | `src/activities/settings/SettingsActivity.cpp`, `src/activities/settings/NetworkActivity.cpp`, `src/main.cpp` |
| `notes` | the Notes sleep screen | `ambientSleepScreen` | `src/activities/boot_sleep/SleepActivity.cpp`, `src/main.cpp` |
| `almanac` | the Almanac sleep screen | `ambientSleepScreen` | `src/activities/boot_sleep/SleepActivity.cpp`, `src/main.cpp` |
| `registerScreen` | the Register sleep screen (an instrument's schedule) and its Settings > Messages rows; the `register` token. Only with the mailbox, timed wake and a sleep screen, so on the boards that have Notes | `offersRegister` | `src/ambient/AmbientSettings.cpp`, `src/activities/ambient/MessagesActivity.cpp` |
| `scheduledDownloads` | the scheduled poll | - | `src/ambient/AmbientSettings.cpp` |
| `timedWake` | arming a wake timer; the timer-wake path | `armedWakeMicros` | `lib/hal/HalPowerManager.cpp`, `src/main.cpp` |
| `stepTimers` | step timer boot, wake hook and alert | - | `src/main.cpp` |
| `alertLight` | the step-timer alert's light | `alertHasLight` | `src/activities/reader/StepTimerAlertActivity.cpp` |
| `cardIdentity` | the card's copy of the identity (`reader.json`); `/Lamina/network-profile.json` beside it | `writesProfileSummary` | `src/ambient/ReaderIdentityDevice.cpp`, `src/main.cpp`, `src/network/NetworkProfileDevice.cpp` |
| `usbDrive` | advertised only: it mirrors `FREEINK_CAP_USB_MSC`, the build flag that gates USB Drive itself | - | - |
| `usbSetup` | Connect to Mac | `offersConnectToMac` | `src/activities/settings/NetworkActivity.cpp` |
| `wifiOpen` | which profiles validate; the Network screen, the installed profile, its time servers and the summary file; Import from card | `offersNetworkProfile` | `src/activities/settings/SettingsActivity.cpp`, `src/network/NetworkProfileDevice.cpp` |
| `wifiPsk` | the same, for a password profile | `supportsWifi` | `src/network/NetworkProfileDevice.cpp` |
| `wifiEapTls` | the same, for a certificate profile; Import from card never offers it (a key never travels on the card) | `supportsWifi` | `src/network/NetworkProfileDevice.cpp` |
<!-- /capability-gates -->

The network features ask these helpers, never the model: `offersNetworkProfile` (the Network screen, the installed profile
and everything that reads it), `writesProfileSummary` (`/Lamina/network-profile.json`), `offersCardImport` (Import from
card), `offersConnectToMac` (the USB setup link) and `offersConnectionCheck` (Check connection). Tested in
`test/plaque_board`, against every real board profile (X4 Pro, X4 Classic, C3 X4/X3, Sticky, Paper Mono) and with single
capabilities taken away, and in the glue tests with the real `NetworkProfileDevice` on an unsupported board: a profile left
in NVS is not seen, nothing is installed or imported, no summary file is written.

## Gate audit

Every board check in `src/` and `lib/`, and what it became. Feature gates now ask a capability;
hardware gates stay with the SDK.

| file:line | old condition | new condition | reason |
|---|---|---|---|
| `lib/hal/HalPowerManager.cpp:72` | `BoardConfig::isX4Pro() && timerMicros > 0` | `plaque::armedWakeMicros(board().caps, timerMicros) > 0` | timed wake is a feature (ambient poll, step timer) |
| `src/main.cpp:405-426` (`enterDeepSleep`) | `const bool x4Pro = isX4Pro()` → `resolveSleepBook`, `methodStepOpen`, `planSleep` | `caps.method` | Method sleep that keeps the step is a feature |
| `src/main.cpp:419` | `x4Pro && useAsSleepScreen && hasContent()` | `plaque::ambientSleepScreen(caps) && ...` | Notes/Almanac sleep screen is a feature |
| `src/main.cpp:619` | `isX4Pro() && cause == TIMER` | `caps.timedWake && cause == TIMER` | the ambient/step-timer wake path |
| `src/main.cpp:626` | `if (isX4Pro())` step timer boot + wake hook | `if (caps.stepTimers)` | step timers are a feature |
| `src/main.cpp:711-718` | `if (isX4Pro())` ambient settings, `readerUuid()`, message state | `if (caps.mailbox)` settings/state; `if (caps.cardIdentity) ambient::syncReaderCard()` | mailbox and the card identity are features; reader.json now written every boot |
| `src/main.cpp:1192` | `if (isX4Pro()) startStepTimerAlertIfDue()` | `if (board().caps.stepTimers)` | step timers are a feature |
| `src/activities/settings/SettingsActivity.cpp` (the Clock row) | `halClock.isAvailable()` | `halClock.isAvailable() \|\| plaque::offersClockReset(caps)` | Reset the clock check is the way out of a bad floor: it must not depend on the RTC probe |
| `src/ambient/AmbientSettings.cpp:17` | `isX4Pro() && useAsSleepScreen && scheduledPolling && configured()` | `caps.scheduledDownloads && ...` | scheduled polling is a feature |
| `AmbientSettings::registerSelected()` | `isX4Pro() && useRegisterScreen` (the Register branch) | `plaque::offersRegister(board().caps) && useRegisterScreen` | Register is a feature of the boards that have Notes, not of the Pro |
| `AmbientSettings::pollingEnabled()` | `isX4Pro() && scheduledPolling && configured()` (the Register branch) | `caps.scheduledDownloads && scheduledPolling && configured()` | the same gate as `wakeTimerMicros()`: scheduled polling is a feature |
| `src/main.cpp` (`enterDeepSleep`) | `x4Pro && (registerSleep \|\| note)` (the Register branch) | `plaque::ambientSleepScreen(caps) && (registerSleep \|\| note)` | Register and the note are both mailbox frames: one sleep-screen gate |
| `src/activities/boot_sleep/SleepActivity.cpp` | `isX4Pro() && registerSelected() && drawPrepared()` (the Register branch) | `registerSelected() && drawPrepared()` | `registerSelected()` asks the capability itself |
| `src/activities/ambient/MessagesActivity.cpp` | (none: nine rows, then twelve with Register) | the three Register rows only where `offersRegister(caps)` (`messagerows::rowsFor`) | Messages is reachable on every board with the mailbox |
| `src/ambient/ReaderIdentityDevice.cpp:126` | `!mirrored && isX4Pro()` | `!mirrored && caps.cardIdentity && cardWritable()` | the card mirror is a feature; never while USB Drive owns the card |
| `src/activities/boot_sleep/SleepActivity.cpp:513` | `isX4Pro() && useAsSleepScreen && ...` | `plaque::ambientSleepScreen(board().caps) && ...` | Notes/Almanac sleep screen |
| `src/activities/home/HomeActivity.cpp:240` | `hasMethod = isX4Pro()` | `hasMethod = board().caps.method` | Method on Home |
| `src/activities/settings/SettingsActivity.cpp:101` | `if (isX4Pro())` Messages row | `if (board().caps.mailbox)` | Settings › System › Messages |
| `src/activities/reader/StepTimerAlertActivity.cpp:76,90` | `Frontlight.present()` | `alertLight_ = plaque::alertHasLight(caps, Frontlight.present())` | the alert's light pulse only where Plaque alerts with a light |
| `src/activities/reader/StepTimerAlertActivity.cpp:236` | "Press any key or tap to continue" | `promptsTouch(caps)` ? same : "Press any key to continue" | no touch wording on Classic |
| `src/activities/reader/StepTimerView.cpp:86` | "Tap to stop[ · ends %s]" | `promptsTouch(caps)` ? same : "Confirm: stop[ · ends %s]" | key wording on Classic (Confirm opens the menu with Stop in it; the first wording, "Press Confirm to stop · ends 2:32 PM", did not fit the box) |
| `src/activities/settings/AboutActivity.cpp:88-127` | (none) | Model row and labelled station MAC when `model != Unsupported`; Touch/Frontlight rows only where the Plaque reader has them | Settings › About; the Classic never mentions touch or light |
| `src/main.cpp:259`, `src/main.cpp:1081` | `isX4Pro() && doubleClickPwrLight` | **kept** | the X4 Pro's power double-click toggles its light: Pro hardware |
| `src/MappedInputManager.cpp:349` | `isX4Pro() && doubleClickPwrLight` (inside `FREEINK_CAP_TOUCH`) | **kept** | same Pro shortcut's click window |
| `src/SettingsList.h:502` | `!isX4Pro()` erase "Double-Click Power for Light" | **kept** | Pro hardware |
| `src/SettingsList.h:251` | `#if FREEINK_CAP_FRONTLIGHT` Restore Light on Wake | **kept** | compiled out on Classic |
| `src/SettingsList.h:353,509-544`, `src/activities/settings/SettingsActivity.cpp:80-98` | `FREEINK_CAP_TOUCH`, `hasTouch()`, `hasHomeKey()` | **kept** | touch/Home-key settings hidden on Classic; Remap Front Buttons shown |
| `src/main.cpp:601`, `src/main.cpp:700` | `isX4Pro() \|\| isX4Classic()` recovery key = Down | **kept** | GPIO0 boot strap on both S3 boards |
| `src/main.cpp:224` | `FREEINK_CAP_TOUCH && hasTouch()` skip Join Network restart | **kept** | touch/frontlight rails |
| `src/main.cpp:824` | `FREEINK_DEVICE_X4PRO \|\| X4CLASSIC \|\| ...` stay awake after USB power | **kept** | USB Serial/JTAG after USB Drive |
| `src/activities/settings/SettingsActivity.cpp:70` | `isX4Pro() \|\| isX4Classic()` hide fading fix | **kept** | panel waveform |
| `src/activities/reader/EpubReaderActivity.cpp:59` | `isXteinkDevice() \|\| isX4Pro() \|\| isX4Classic()` | **kept** | panel refresh choices |
| `src/MappedInputManager.cpp:20,332,335,340` | `hasHomeKey()`, `Frontlight.present()` | **kept** | Home key and light-panel gestures absent on Classic |
| `src/activities/reader/EpubReaderMenuActivity.cpp:52`, `src/activities/util/FrontlightPanelActivity.cpp:277,365` | `Frontlight.present()` | **kept** | light controls hidden (false on Classic, `FrontlightManager.h:63-76`) |
| `src/network/FirmwareBoardTag.cpp:10-13` | `FREEINK_DEVICE_X4PRO` → `x4pro`, `X4CLASSIC` → `x4c` | **kept** | the SD installer's board tag |

The Wi-Fi and clock adapters (`method::DeviceWifi`, `DeviceClock`, now in `MethodNetDevice.cpp`; `DeviceHttp` in
`MethodDevice.cpp`), `ambient::connectSavedWifi()/disconnectWifi()` (now in `NetworkJoinDevice.cpp`), `WifiCredentialStore`,
`lib/TrustedTime`, `MessageTls` and `HttpDownloader` carry no board gate: they run on whatever Wi-Fi the board has. What the
network *profile* adds on top of them (the Network screen, the installed profile, card import, Connect to Mac, the summary
file) is gated by capability (the table above).

## Keys only

Every Plaque action on the Classic runs on its keys (side Up/Down, power, bottom Left, Right,
Confirm, Back; `BoardConfig.h:1810`, HalGPIO indices `BTN_BACK..BTN_POWER`). Verified by reading:

| Action | Key route | Where |
|---|---|---|
| Home → Method | Up/Down (or Left/Right) to the row, Confirm | `HomeActivity` menu, `hasMethod` |
| Shelf: open a book | Up/Down, Confirm | `MethodActivity::handleButtons` (`MethodActivity.cpp:457`) |
| Shelf: check now | Up from the first row onto the status line, Confirm | `MethodActivity::navigateButtons` (ring), `recheck()` |
| Shelf: remove | hold Confirm 1 s → Keep/Remove with Left/Right/Up/Down, Confirm; Back cancels | `MethodActivity.cpp:447`, `MethodRemoveDialog.h:82-92` |
| Shelf → Home | Back | `MethodActivity::onBackButton` |
| Page turns (XTC) | side Up/Down (`PageBack`/`PageForward`), or front Left/Right | `ReaderUtils.h:72-83` |
| Steps list | Confirm on a page without a timer | `XtcReaderActivity::handleFormatInput` |
| Start 5:00 timer / Stop / Steps | Confirm opens the menu; Up/Down, Confirm; the stop question by Left/Right, Confirm | `XtcReaderActivity::openTimerMenu`, `StepTimerDialog.h:112-126` |
| Answer the alert | any key, on release, after 700 ms | `StepTimerAlertActivity::loop` (`gpio.wasAnyReleased()`) |
| Back from a Method book | Back (to the shelf) | `ReaderActivity` |
| Messages settings | Up/Down, Confirm; keyboard by keys | `MessagesActivity` (`UiListActivity`) |
| Settings › About | Up/Down, Back | `AboutActivity` (`UiListActivity`) |
| Sleep / wake | hold power / power | `main.cpp` loop, `PowerManager::armPowerButtonWakeup` |
| Cancel a timer wake's book check | any key pressed during the check (its press edge, from the debounced digital inputs); a key already down when the check began counts only if it is the power key or on the button ladder (`HalGPIO::rawInputActive()` reads those two and not the Classic's digital keys) | `MethodWakeRun.cpp:60-72`, `method::wake::PressWatch` |

No Plaque path needed the Home key or touch only, so no new key route was added. Touch-only
wording was: the running timer's "Tap to stop" and the alert's "Press any key or tap to
continue"; on the Classic they say "Confirm: stop" and "Press any key to continue"
(`promptsTouch()`).

## Step-timer alert without a frontlight

On the Classic the alert is the inverted timer box (or banner), drawn exactly as on the Pro, and
the reader holds itself awake for a key press (`preventAutoSleep()` until the 10-minute
`ALERT_LIMIT_MS`, after which a reader that woke by itself goes back to sleep with the finished
timer on the panel, as on the Pro). `alertLight_` is false, so the alert never reads the
frontlight's state, never sets its brightness and never switches it: `steptimer::alertLight()`
runs with `quiet` and returns `on == false` for every frame (`test/plaque_board`). Even if it
were asked, `FrontlightManager` is compiled without its driver on the Classic
(`FREEINK_CAP_FRONTLIGHT` is 0, `FrontlightManager.cpp:417-428`; `present()` is false,
`FrontlightManager.h:63-76`), so every `Frontlight.*` call in `main.cpp` (`cardlessDeepSleep`,
`Frontlight.begin`) is a no-op there. No string mentions light, sound or vibration.

## Hardware behaviour: verified from the SDK, and what is not

Verified by reading the SDK at `freeink-sdk` bbd528c and this firmware:

- **Wake sources.** Power is GPIO3, active-low (`BoardConfig.h:1810`, same as the Pro at
  `BoardConfig.h:1658`); deep sleep arms it as RTC ext1 `ANY_LOW` on the S3
  (`PowerManager.cpp:34-65`) and arms the timer when Plaque asks
  (`HalPowerManager.cpp:70-77`). `PowerManager::deepSleep()` isolates the pads and enables the
  deep-sleep holds before `esp_deep_sleep_start()` (`PowerManager.cpp:125-130`).
- **GPIO1 rail.** The Classic's `power.latch0` is GPIO1 (`BoardConfig.h:1833`), driven HIGH at
  boot by `holdPowerRails()` (`BoardConfig.h:2099-2120`, `main.cpp:557`) and held HIGH through
  sleep (`HalPowerManager.cpp:107-116`; it is not GPIO13, so the C3 battery-off path does not
  apply).
- **Display reset across sleep.** The Classic's display has no switched power enable
  (`BoardConfig.h:1788`), so its RESET (GPIO10) is held HIGH through sleep
  (`PowerManager.cpp:114-115`), and `EpdBus` / `XteinkDetect` release that hold before pulsing
  it (`EpdBus.cpp:115`, `XteinkDetect.cpp:146,196`).
- **SD power (GPIO6).** Active-low (`BoardConfig.h:1793`); held HIGH (off) through sleep
  (`PowerManager.cpp:119`); on the next boot the SDMMC mount releases the hold and pulses it
  HIGH 80 ms / LOW 120 ms per attempt, four attempts, with a real sector read each
  (`SdmmcBlockDevice.cpp:85-118`). `main.cpp` retries the mount twice more when a step timer
  is due (`main.cpp:644`).
- **The panel through a timer wake.** The timer-wake path in `setup()` (`main.cpp:736-795`)
  draws only when `sleepframe::planWake()` allows a redraw, which it never does while a Method
  step is on the panel. Without a redraw no display code runs: no `display.begin()`, no
  controller probe (`setupDisplayAndFonts`, `main.cpp:510`), no refresh. E-ink keeps the
  image without power. When the display does start (a button wake, the alert), the drivers'
  `begin()` reset and initialise the controller without a refresh command
  (`Uc8179Driver.cpp:219-234`, `Uc8279X4Driver.cpp:285-296`, `Ssd1677Driver.cpp:192-195`) and
  the first paint is a full one (`_needFullClear`), which repaints the retained Quick Resume
  frame loaded from `/.crosspoint/sleep_frame.bin`. This is the same code as the Pro's; the
  only Classic difference is that its UC8279 init skips the PLL command
  (`Uc8279X4Driver.cpp:272-276`). The controller is chosen from NVS `hw_calib/screenType`, else
  a VER probe that resets the controller (`XteinkDetect.cpp:440-495`), and only when the
  display starts.
- **Clocks.** The step timer counts on `esp_timer` awake and on the system time across a
  sleep (`StepTimerService.cpp:24-36`), read at boot before anything can step it
  (`main.cpp:571`), the same chip clocks as the Pro. The BM8563 at 0x51 is in the Classic's
  profile (`BoardConfig.h:1829`, `FREEINK_CAP_RTC`, `BoardConfig.h:302-306`) and is used for
  the status-bar clock only (`HalClock::localTime`), never to set the system time.
- **Keys.** Seven discrete active-low buttons, back 9 / confirm 8 / left 5 / right 2 / up 0 /
  down 7 / power 3, `InputStyle::DigitalButtons`, no touch (`BoardConfig.h:1778-1816`),
  read by `InputManager::getDigitalState()` (`InputManager.cpp:191-196`, `335`); checked against the
  real profile in `test/plaque_board` (`PlaqueBoardProfile_x4c`).
- **Firmware installer.** Each image carries one `CROSSPOINT-BOARD-V1:<board>;` tag
  (`FirmwareBoardTag.cpp:10-13`); the SD installer refuses an image whose tag names another
  board (`FirmwareFlasher.cpp:210-216`): a Pro image on a Classic, and the other way round
  (`test/firmware_board_tag`, and `test/check_firmware_image.py` on the built images).

Not verifiable without a reader (each fails safe as noted):

- That holding GPIO1 and RESET across a timer wake leaves the Classic's panel image untouched
  (the Pro's identical path was confirmed on a device; the Classic's is not yet). If it did not,
  the step would be lost from the panel on a timer wake, but the button wake still repaints it
  from the saved frame.
- That the timer wake actually fires on the Classic (GPIO3 ext1 plus the RTC timer is
  standard S3 behaviour, and the Pro does it), and its sleep current with the GPIO6/GPIO10
  holds.
- USB Drive (MSC) and USB Serial/JTAG on the Classic (contract: "yes (verify)").
- The Classic's real partition table (below).
- The SDK's own list of what is still pending hardware for the Classic (`freeink-sdk/docs/xteink-x4c-support.md`,
  `BoardConfig.h` ~1770; each is on the acceptance checklist, section 0b):
  - **Panel orientation and flip.** The profile says `NO_FLIP`, "panel mount transform pending hardware; native SSD1677
    scan is 800x480 landscape". Plaque's portrait page may come out mirrored, rotated or upside down on a real Classic.
  - **Bezel insets.** The scrollbar and edge inset values are the X4's historical ones for top and bottom, "pending
    measurement"; text or the key hints may sit too close to, or under, the bezel.
  - **Charge status (STAT, GPIO21).** The polarity (active-high is assumed) is unconfirmed: the charging indicator and
    anything that reads it can be inverted.
  - **GPIO4.** Configured as an input but not interrupt-attached, so not a key; its role is unknown.

## Audit results (source and installed SDK, no hardware)

- **USB Drive on the Classic.** MSC is enabled only by the build flag `FREEINK_CAP_USB_MSC`, which the `x4c` envs set
  (`platformio.ini`, same as `x4pro`). Nothing gates the File Transfer menu entry, `UsbDriveActivity`,
  `ActivityManager::goToUsbDrive` or `HalStorage` by board (`NetworkModeSelectionActivity.cpp:17-33`,
  `ActivityManager.cpp:261`, `HalStorage.cpp:53-103`). `HalStorage.cpp:53` is a compile-time `#error` unless the storage
  profile is SDMMC, and the x4c build passes it. Both built images carry the same MSC code (17 matching strings each).
  The SDK lists USB D-/D+ as GPIO19/20 on both boards. What remains unverifiable is behaviour on a real Classic: the
  host enumerating it, and the SD power gate (GPIO6) across a mount.
- **EAP server certificate dates.** The installed prebuilt SDK has `CONFIG_MBEDTLS_HAVE_TIME_DATE` not set
  (`sdkconfig:2838`), so mbedTLS inside the supplicant cannot judge dates whatever the clock says. This is the same
  reason `MessageTls` carries its own date-checking verifier. The supplicant's verifier cannot be hooked from public
  APIs, and rebuilding the SDK would change the X4 Pro's verified build, so it stays a documented limit: the CA chain
  and the server name are required, dates are not judged. Use short-lived, revocable client certificates.
- **Retained clock.** The BM8563 is written only from a judged SNTP answer: `HalClock::writeUtc` is called from the clock
  record (`ClockProvenanceDevice.cpp`) and nowhere else, `HalClock::syncFromNTP` hands its answer to the record instead of
  writing the chip, and `test/test_clock_chip_writes.py` holds that (and that only `lib/TrustedTime` starts SNTP, so every
  sync runs the profile's servers first). The chip is the witness (`clockprov::judgeSync`): it is set from an earlier sync
  and counts on its own crystal, so a sync it agrees with is recorded and confirmed, one it contradicts (once confirmed) is
  used for the session and recorded nowhere, and one nothing can witness is recorded but moves the persisted floor by a
  minute at most. The retained clock is held to the **proven floor** (`trustedtime::provenFloor()`: the persisted floor plus
  what confirmed syncs and the chip raised it to; never the system clock, which runs up to 2 % fast through a sleep), with a
  tolerance of 10 minutes: a chip set back further than that is not trusted. The chip is written with the sync's own
  second, never the floor. Settings › System › Clock › Reset the clock check clears the floors and the record, withdraws the HTTPS permission a sync
  gave, and sets the system clock to zero so the reader reads as having no time (`plaque::offersClockReset`: the Clock
  screen is offered wherever the board has the mailbox or a network profile, whether or not the RTC chip answered its probe,
  because a bad floor must not need a flash to clear). The first sync of a reader with no clock record may be up to a day
  behind a floor an earlier firmware left ahead of true time and lowers the floor to itself, once (the chip, if it
  answers, must agree; once a record exists a backward sync is refused as ever). The 2 % drift figure of
  the internal oscillator is an assumption, to be measured (acceptance checklist 0b); at 2 % the internal clock reaches about
  8.3 hours, the chip 7 days. The Method shelf has the same retained-clock fallback as Check connection and the Notes poll,
  under the same rules.
- **Alert without a frontlight.** `StepTimerAlertActivity` touches the light only when `alertLight_`; its restore is
  guarded by `lightTouched_`; the other `Frontlight.*` calls are compiled-out no-ops on the Classic.
- **USB receive queue.** Read in the installed Arduino core (`cores/esp32/HWCDC.cpp`, `hw_cdc_isr_handler`): the receive
  interrupt reads the whole 64-byte FIFO packet and pushes it byte by byte into the queue with `xQueueSendFromISR`; when the
  queue is full it `break`s and the rest of the packet is **dropped**. It never masks the receive interrupt and never
  leaves the endpoint unread, so there is **no backpressure**: the host is never told to wait (no NAK), and the Mac, which
  sends its DATA lines unpaced, can overrun a small queue in a few tens of milliseconds, for instance while the loop task
  waits out a panel refresh for the render lock. The firmware therefore sizes the queue for a whole legal transfer while
  Connect to Mac is open (`netprov::RX_QUEUE_BYTES`, 24 KB, computed from the protocol's limits and tested), then 8 KB, then
  the core's 256-byte default, and logs which it got; the wire protocol is unchanged. The swap deletes the old queue before
  making the new one, under a critical section. Entering and leaving the screen repeatedly is on the device checklist.

## Partitions

`partitions-x4c.csv` is the table `freeink-sdk/docs/xteink-x4c-support.md` documents: nvs
0x9000/0x5000, otadata 0xE000/0x2000, app0 0x10000/0x7E0000, app1 0x7F0000/0x7E0000, spiffs
0xFD0000/0x14000, coredump 0xFE4000/0x1C000 (sizes from the gaps; it equals the Pro's numbers but
is its own file). **It has not been read from a real X4 Classic.** Before any flashing, read
0x8000 (0xC00 bytes) from the reader and compare. The x4c, x4c-gh_release and x4c-gh_release_rc
envs build against it; `partitions-x4pro.csv` and the x4pro envs are unchanged.
`test/test_partitions.py` checks the file against the SDK document, the table's soundness, the
caveat, the Pro's table, and that built images fit the slot.

## Identity on USB and in reports

The x4c envs introduce themselves as `Plaque X4 Classic` by `Solanum`, like `Plaque X4 Pro`. The
development env's version comes from `scripts/git_branch.py` as for the Pro:
`1.6.5-x4c+<sha>[.dirty][.<branch>]`, at most 40 printable characters (the `X-Lamina-Firmware`
limit). Release envs keep `${crosspoint.version}` as the Pro's do.

## `/Lamina/reader.json` (schema 2)

Written at every boot on both boards, after the card mounts and before any network
(`ambient::syncReaderCard()`, `main.cpp:718`), only when the content differs (a read and a
compare otherwise), never while USB Drive owns the card. Fields: `reader_uuid`, `firmware`,
`schema` 2, `model`, `model_name`, `station_mac` (`esp_read_mac(ESP_MAC_WIFI_STA)`, the address
Wi-Fi registration needs), `hotspot_mac` (`ESP_MAC_WIFI_SOFTAP`, the File Transfer hotspot,
never registered), `wifi_bands` (`["2.4GHz"]`), `capabilities`. An address the chip will not
give is left out. A schema 1 file, or one missing or differing in any of these, is rewritten;
unknown fields are ignored. Settings › About shows the model and the station MAC, labelled
"Wi-Fi MAC · The address to register for Wi-Fi" (the MAC row was already the station address,
`esp_read_mac(ESP_MAC_WIFI_STA)`); on the Classic it leaves out the Touch and Frontlight rows. The
`X-Lamina-*` headers are unchanged.

## The network profile: storage, joining, and what it does not reach

(The contract is `SPEC-UNIVERSITY-WIFI.md` sections 4 to 7; this is what the firmware does about it.)

- **Storage.** One profile, in NVS (`plaque_net`), two slots and a pointer. Removing or replacing a profile *deletes* its
  entries with `nvs_erase_key`, which is logical: the entry is marked deleted and the reader's APIs stop returning it, but the
  bytes (a PSK, a private key) stay in the flash page until NVS reclaims it, and flash and NVS encryption are off. A
  deleted key is not scrubbed from the chip, and anyone with the reader and a USB cable can read the flash. Say so wherever
  removal is described; the install question says it before a certificate profile is replaced ("Its key is deleted from
  the reader.") and that a stored key or password is unencrypted.
- **Who joins through the profile.** The Method shelf's check, the Messages check, the Register check (View the register >
  Check the register now), a timer wake (Notes, Almanac, the Register poll, the book check), the sleep-event delivery
  before deep sleep (`main.cpp`, `deliverSleepPluginEvents`) and Check connection: all through `netjoin`
  (`NetworkJoinDevice.cpp`, `MethodNetDevice.cpp`; the two Messages screens in `MessageViewActivity.cpp` and
  `RegisterActivity.cpp`, which wait `netjoin::interactiveProfileJoinMs()` for the profile before offering the saved
  networks). A wake gives the profile 10 s (25 s for EAP-TLS) and the
  last-used saved network 10 s, tries first whichever joined last (kept in RTC memory), and the step-timer poll budget
  (`steptimer::pollBudgetMs`) grows with it: 150 s with no profile, 160 s or 175 s with one. An EAP-TLS join sets the
  scan threshold to WPA2-Enterprise, so an open or PSK network of the same name is never joined.
- **Who does not.** File Transfer, OPDS, KOReader Sync and the clock-sync screen start Wi-Fi through
  `WifiSelectionActivity`, which knows the saved networks only (`WIFI_STORE`, passwords). On a campus network that exists only
  as a profile (an EAP-TLS network, or one the person chose to keep out of the saved list) those features cannot get
  online, and nothing here changes that: they were not rebuilt. Use a saved network for them, or
  none; the limit is stated in `SPEC-UNIVERSITY-WIFI.md` section 6.
- **Time.** The profile's SNTP servers apply to every sync (`message_tls::prepareClock()` and `beginClockSync()` apply them
  themselves, so a new caller such as the Register poll gets them), and `use_dhcp_time` is switched on before the join.

## Register: how it is gated, joined and budgeted

Register (`docs/ambient-frame.md`, the Register section) is a mailbox frame like Notes and Almanac, so it is offered on the
same boards, by capability and not by model: `plaque::offersRegister(caps)` needs the `registerScreen` bit, the mailbox,
timed wake and a sleep screen. The bit exists so that the `register` token in `reader.json` and the gate have one
definition; the helper is what code asks. It is asked in two places: `AmbientSettings::registerSelected()` (every
decision about the screen, the wake and the poll goes through it, so a `use_register_screen` left in the settings file of
a board without it is kept and never acted on) and the rows of Settings > System > Messages (`messagerows::rowsFor`). On
the X4 Pro and the X4 Classic it is true.

- **Joins.** The scheduled Register poll is the Notes poll's wake path: `ambient::connectSavedWifi()`, which is
  `netjoin::joinForWake()`, with the wake order (the network that joined last first) and the per-candidate budgets above.
  "Check the register now" joins as the Messages check does (the profile, then the saved networks). A boundary wake, which
  only redraws, joins nothing.
- **Time.** `RegisterProvider::fetch()` calls `message_tls::prepareClock()` before it opens a connection, and that applies the
  installed profile's time servers and DHCP option itself. `test/test_register_seams.py` holds the order in the source
  (the clock, then the request; the fetch inside the wake join), and `test/network_profile/glue` runs the same calls
  against the fake radio.
- **Budget.** A Register poll is the join, one bounded request for the schedule (the note poll makes two) and the Method
  book check, so `steptimer::pollBudgetMs(wakeJoinWorstMs(...))` (150 s, 160 s with an open or PSK profile, 175 s with an
  EAP-TLS one) covers it with room to spare. The skip near a step timer's end is made for every ambient timer wake, a
  Register boundary wake included (it is the same wake), with the budget of the profile installed now
  (`netjoin::wakeJoinWorstMsNow()`); a reader with no profile skips for 150 s exactly as the Register branch alone did.
- **Classic.** The only difference in what Register says: where the Pro's full-day footer reads "Right or tap: next", a
  board without touch reads "Right: next" (`promptsTouch`). Nothing about Register has run on a Classic.

## What a device test must confirm

On each board (X4 Pro and X4 Classic), from a card with a Method book carrying a timer:

1. Boot: `/Lamina/reader.json` appears (USB Drive) with the board's `model`, both MACs, and the
   capability list (Pro: with `touch` and `frontlight`; Classic: without). The station MAC
   matches Settings › About. A second boot logs no write.
2. Classic only: Home shows Method; open the shelf, a book, the Steps list, start the 5:00
   timer, stop it, remove a book, check now, all with keys. The running box says "Confirm: stop ·
   ends …" and fits; Settings shows Messages, no touch or light settings, and Remap Front Buttons.
3. Sleep inside the book with the timer running (power): the panel keeps the step with "Ends …".
   With scheduled polling on and a note waiting, let a timer wake fetch it: the step stays on the
   panel. Then let the timer end: the reader wakes, shows the inverted box, stays awake, and a key
   answers it. Pro: the light pulses as before. Classic: no light, nothing waits on one.
4. A timer wake while the reader is in a pouch (no key): it goes back to sleep without drawing.
5. USB Drive on the Classic: the card mounts on the Mac; leaving it reboots to Home.
6. SD card firmware update: a Pro image on the Classic and a Classic image on the Pro are refused
   ("wrong board"); each board's own image is accepted.
7. Before flashing the Classic at all: read its partition table at 0x8000 and compare with
   `partitions-x4c.csv`.
