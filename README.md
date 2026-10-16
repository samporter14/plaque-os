# Plaque OS

Plaque OS is e-reader firmware for the **Xteink X4 Pro** (ESP32-S3). It is a fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) 1.6.5 that adds lab-bench features on top of
a full EPUB reader: protocols you can follow step by step, an instrument schedule on the sleep screen, and notes sent
from a phone or a Mac.

> ## Status: beta 0.1. Read this before you flash anything.
>
> - **Beta 0.1 has only been run on the author's X4 Pro.** Most of it has never run on any reader; the repository's
>   tests are host tests and the build is checked on a Mac. [docs/PROJECT-STATUS.md](docs/PROJECT-STATUS.md) says what
>   is and is not verified.
> - **Flashing can brick a device.** You are on your own. Nothing here is a supported product.
> - **Recovery is USB flashing or the SD-card firmware update** (Settings > System > SD Card Firmware Update, which
>   installs a `.bin` from the card). If your device is USB-locked, see the warning below before you do anything.
> - **Back up your own app slot first.** Read your partition table, save the slot you are about to overwrite, and keep
>   the file ([Flash](#flash)). Do not publish that backup: it can hold vendor firmware and your device's stored data.
> - The **X4 Classic** build exists but has **never run on real Classic hardware**. Do not flash it on anything yet.
> - **Not affiliated** with Xteink or with CrossPoint Reader (see [Relationship to CrossPoint](#relationship-to-crosspoint)).

## What differs from CrossPoint

Plaque OS keeps CrossPoint's reader (EPUB, library, fonts, Wi-Fi file transfer, OPDS, KOReader sync, and the rest;
see [USER_GUIDE.md](USER_GUIDE.md)). It adds:

- **Method.** Protocols as books: a procedure published to the reader as a paged book, one step per page, with step
  timers that keep running while the reader sleeps, wake it at zero and pulse the light (on a board with no frontlight, the step's box inverts). The step stays on the panel
  through sleep.
- **Register.** An instrument's schedule from its Google Calendar as the sleep screen: free or in use, who and until
  when, what is next, today and tomorrow. It redraws at each booking boundary and at a poll interval.
- **Messages.** A note, drawing or photo sent to the reader through a mailbox and shown as the sleep screen, checked
  on a timer or on demand over verified HTTPS ([docs/ambient-frame.md](docs/ambient-frame.md)).
- **University Wi-Fi profiles.** One network profile, set up from the Plaque Mac app over USB or from the SD card: an
  open network, WPA2/WPA3 with a pre-shared key, or **EAP-TLS** (client certificate; no username-and-password methods
  and no captive-portal login, on purpose). Spec: [docs/SPEC-UNIVERSITY-WIFI.md](docs/SPEC-UNIVERSITY-WIFI.md).
- **X4 Classic capabilities.** A capability layer (`lib/PlaqueBoard`) asks what a board can do instead of which board
  it is, so the same features run on the buttons-only X4 Classic. **Untested on real Classic hardware**
  ([docs/x4c-support.md](docs/x4c-support.md), [docs/X4C-WIFI-DEVICE-ACCEPTANCE.md](docs/X4C-WIFI-DEVICE-ACCEPTANCE.md)).
- **No network update checks.** CrossPoint's check against its own releases is removed, because an upstream image
  would replace Plaque OS. Firmware is installed over USB or from a `.bin` on the SD card, and
  `test/test_no_upstream_updates.py` and `test/check_firmware_image.py` guard this. The reader still contacts hosts you
  choose or enable: your mailbox, NTP servers for the clock, and, only if you use those features, CrossPoint's
  KOReader sync server (the default for KOReader Sync) and the CrossPoint font catalog on GitHub (Manage Fonts).

## Supported boards

| Board | Env | Status |
|---|---|---|
| Xteink X4 Pro (ESP32-S3, touch, frontlight) | `x4pro` | The only board it has run on (the author's). |
| Xteink X4 Classic (ESP32-S3, buttons only) | `x4c` | **Not tested** on real hardware. |

The base reader's other CrossPoint environments (`default` for the ESP32-C3 X3/X4, `sticky`, `papermono`) are still in
`platformio.ini` but are not tested in this fork.

## Build

You need Git, Python 3.12, and PlatformIO (the pioarduino core pinned in `requirements-build.txt`). A connected reader
is not needed.

```sh
git clone https://github.com/samporter14/plaque-os
cd plaque-os
git submodule update --init          # freeink-sdk, pinned at bbd528c
```

On Apple-silicon macOS the helper script installs the build dependencies into its own environment under `build/` and
builds the X4 Pro image (it takes no arguments):

```sh
bash scripts/build_x4pro_macos.sh
# optionally reuse an existing environment:
#   AMBIENT_VENV_DIR=/path/to/venv PLATFORMIO_CORE_DIR=/path/to/pio-core bash scripts/build_x4pro_macos.sh
```

Or, anywhere, with your own environment:

```sh
python3.12 -m venv .venv
.venv/bin/python -m pip install -r requirements-build.txt
.venv/bin/pio run -e x4pro          # the image is .pio/build/x4pro/firmware.bin
.venv/bin/pio run -e x4c            # the Classic image; run it after the first one finishes, never together
python3 test/check_firmware_image.py --board x4pro .pio/build/x4pro/firmware.bin
python3 test/check_firmware_image.py --board x4c .pio/build/x4c/firmware.bin
```

Notes:

- The first build downloads the pinned Espressif toolchain and takes several minutes. Run **one `pio` environment at a
  time**: two at once race on `.cache/.sconsign`.
- Build from a committed tree. The version string carries `.dirty` when the tree is not clean, and the guards expect a
  clean one. The string is `1.6.5-x4pro+<short SHA>[.dirty].<branch>` (`scripts/git_branch.py`) and shows in
  Settings > About.
- `check_firmware_image.py` checks the board tag, the USB product string, that no upstream update strings are in the
  image, and that every host name in it is allowed. `--board x4pro|x4c|none` is required for a copy of an image
  anywhere else.
- PlatformIO needs network access on the first build (platform, toolchain, libraries).

### Tests

```sh
# host tests (needs CMake; googletest is fetched on first configure)
cmake -S test -B build/htest
cmake --build build/htest -j8          # let it finish before ctest
ctest --test-dir build/htest -j8

# the Python guards
for f in test/test_*.py; do python3 "$f"; done
```

The suites that parse JSON need ArduinoJson, which exists after one `x4pro` build (`.pio/libdeps/x4pro`). The
`network_profile` suite and `test/message_tls` need a host Mbed TLS 3.6.6 with `MBEDTLS_HAVE_TIME_DATE` disabled: pass
`-DMBEDTLS_SOURCE=... -DMBEDTLS_BUILD=...` (see [docs/message-tls-candidate.md](docs/message-tls-candidate.md)), or
`-DNETWORK_PROFILE_FETCH_MBEDTLS=ON` for the `network_profile` suite. Formatting: `./bin/clang-format-fix -g` with
clang-format 21. More in [docs/contributing/](docs/contributing/README.md).

## Flash

The X4 Pro firmware goes into the **app1** slot at `0x7F0000`. **This applies to the X4 Pro's own partition table
only.** Before you write anything, read your reader's partition table and compare it with `partitions-x4pro.csv`
(app0 at `0x10000`, app1 at `0x7F0000`, each `0x7E0000` long). If it differs, stop.

With [esptool](https://github.com/espressif/esptool) 5.x (esptool 4.x spells the commands `read_flash` and
`write_flash`), the reader connected over USB and awake, and `PORT` set to its serial port (on macOS something like
`/dev/cu.usbmodem1101`; on Linux `/dev/ttyACM0`):

```sh
# 1. read the partition table and compare it with partitions-x4pro.csv
esptool --chip esp32s3 --port "$PORT" --baud 460800 read-flash 0x8000 0xC00 ptable.bin

# 2. back up the slot you are about to overwrite, and keep the file (do not publish it)
esptool --chip esp32s3 --port "$PORT" --baud 460800 read-flash 0x7F0000 0x7E0000 app1-backup.bin

# 3. write the image you built
esptool --chip esp32s3 --port "$PORT" --baud 460800 --after watchdog-reset write-flash 0x7F0000 .pio/build/x4pro/firmware.bin
```

To go back, write `app1-backup.bin` to `0x7F0000` the same way. There is no web flasher or official release for Plaque
OS: build the image yourself. The step-by-step device test, with what to expect at each step, is in
[docs/DEVICE-TEST.md](docs/DEVICE-TEST.md). Do not flash the `x4c` image on an X4 Pro. Install an X4 Classic image from
the SD card, not with esptool, and only after reading that reader's own partition table.

An external-programmer recovery guide, inherited from CrossPoint and adapted, is in
[docs/fix-bricked-xteink.md](docs/fix-bricked-xteink.md). It needs a dump of your own device.

## USB-locked devices

Some Xteink units bought from third-party stores ship with USB flashing locked from the factory. **You do not need to
worry about this if USB flashing works on your device.** If it is locked, an unlocker tool is needed before any
firmware can be flashed; CrossPoint Reader's project documents one. Plaque OS has not been tested with it.

> ### WARNING: read this before using any unlocker
>
> The only firmwares an unlock tool officially supports are the ones its authors list. Flashing any other firmware on a
> USB-locked device may **permanently brick the device** or leave it **permanently stuck on that firmware with no
> recovery path**. Once USB flashing is locked again, your only way back is an update installed on the device itself,
> and if the firmware you flashed can't do that, **there is no way out**. Plaque OS can: it installs a `.bin` from the SD
> card (Settings > System > SD Card Firmware Update). That is not a promise that an unlocker works with it.

## Mailbox configuration

Messages and Register read a small JSON file on the SD card, `/.crosspoint/ambient-config.json`. You can also fill the
same fields in on the reader under **Settings > System > Messages**. Example (placeholders only):

```json
{
  "base_url": "https://mailbox.example.workers.dev",
  "device_id": "my-reader",
  "device_token": "REPLACE_WITH_THE_READERS_OWN_SECRET_AT_LEAST_32_CHARACTERS",
  "poll_minutes": 60,
  "use_as_sleep_screen": false,
  "scheduled_polling": false,
  "allow_local_http": false,
  "use_register_screen": false
}
```

- `base_url` is the mailbox origin: `https` only (a public certificate the reader trusts, and a correct clock), no path.
  `http` is accepted only for a private-network, `localhost` or `.local` host and only with `"allow_local_http": true`
  (development).
- `device_id` is 1-32 letters, digits, `-` or `_`; `device_token` is the reader's own bearer secret, 32-128 characters
  of the same set. The token is stored in plain text on the card: keep the card private.
- `poll_minutes` is 5-1440 (default 60). Both sleep options and timed polling start **off**.
- The reader also writes `/Lamina/reader.json` on the card (its model, capabilities and station MAC), which the Plaque
  Mac app reads to recognise the reader.

The mailbox is the **Plaque Mac app and its Cloudflare Worker**
([github.com/samporter14/plaque](https://github.com/samporter14/plaque)): the Mac app publishes Method books, the Register
calendars and notes to the Worker, and sets the reader up; the reader only ever polls the Worker with its own bearer
token. Deploy the Worker in your own Cloudflare account (the Plaque repository explains how). For development without
either, `sender-web/` is a small Python mailbox and sender page that speaks the Messages protocol
([sender-web/README.md](sender-web/README.md)).

## Documentation

- [docs/PROJECT-STATUS.md](docs/PROJECT-STATUS.md): what is verified, what is not
- [docs/DEVICE-TEST.md](docs/DEVICE-TEST.md): the short test to run on an X4 Pro
- [docs/ambient-frame.md](docs/ambient-frame.md): Messages, Method sleep and Register, and the wire protocol
- [docs/SPEC-UNIVERSITY-WIFI.md](docs/SPEC-UNIVERSITY-WIFI.md), [docs/x4c-support.md](docs/x4c-support.md),
  [docs/X4C-WIFI-DEVICE-ACCEPTANCE.md](docs/X4C-WIFI-DEVICE-ACCEPTANCE.md)
- [USER_GUIDE.md](USER_GUIDE.md) (inherited from CrossPoint), [docs/webserver.md](docs/webserver.md),
  [docs/webserver-endpoints.md](docs/webserver-endpoints.md), [docs/troubleshooting.md](docs/troubleshooting.md)
- [docs/contributing/](docs/contributing/README.md) and [AGENTS.md](AGENTS.md): the contributor guide

## Relationship to CrossPoint

Plaque OS is a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) 1.6.5 (base commit
`1f77b83` of its `develop` branch, October 2026), which is MIT licensed. The reader engine, library, web server and
much else are theirs, and the hardware layer is the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) (MIT), pinned
as the `freeink-sdk` submodule. CrossPoint's copyright notice is kept in [LICENSE](LICENSE) and credited on the About
screen. If you want a general-purpose open reader firmware, use CrossPoint itself.

**Plaque OS is not affiliated with, endorsed by, or supported by Xteink or CrossPoint Reader.** "Xteink" and "X4 Pro"
name the hardware this runs on. Please do not ask the CrossPoint project for help with this fork.

## Licence

The source is [MIT](LICENSE) (Copyright (c) 2025 CrossPoint Reader organization; (c) 2026 Plaque authors). **Firmware
binaries are different.** They link wolfSSL 5.7.2 (GPL-2.0-or-later or commercial) and other copyleft libraries, so a
binary built from this repository is a combined work that may be distributed only under GPL terms. This repository, at
the commit a binary was built from, with the pinned versions and the build steps above, is its corresponding source.
Read [NOTICE](NOTICE) before you share a binary. Every third-party component, with its licence, is listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), and the licence texts are in [LICENSES/](LICENSES/). **This
repository's releases do not attach firmware `.bin` files**; build your own.

## Contributing

Issues and pull requests are welcome, but this is a small personal fork tested on one device: expect slow answers and
no promises. Start with [docs/contributing/](docs/contributing/README.md). Run the checks listed above, and say which
reader you tested on. Anything that belongs in the general reader is better sent to CrossPoint.

## Security

Please report a vulnerability privately, with GitHub's private vulnerability reporting on this repository (Security >
Report a vulnerability), not in a public issue. See [SECURITY.md](SECURITY.md).
