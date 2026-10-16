# Project status

Plaque OS is beta 0.1, a personal fork of CrossPoint Reader 1.6.5 for the Xteink X4 Pro. This page says plainly what
has and has not been verified. There is no roadmap and no promise of support.

## Verified (on a development Mac)

- Both firmware images build: `pio run -e x4pro` and `pio run -e x4c`, and `test/check_firmware_image.py` passes on
  each (board tag, USB product string, no upstream update strings, every host name allowed).
- The host test suites pass: the gtest suites under `test/` (Method, Register, Messages, step timers, clock
  provenance, network profile and join, reader identity and the rest), the `message_tls` regression against a real
  Mbed TLS with fixtures, and the Python guards `test/test_*.py`.
- The reviewers' fuzzers for the profile parser, card import and the USB setup protocol ran 150 000 iterations each
  under ASan and UBSan without a failure (these fuzzers are not in this repository).

Host tests compile the shipped code against fakes (a fake radio, supplicant, NVS, card and clock). They do not prove
hardware behaviour.

## Not verified

- **Almost everything on a reader.** Beta 0.1 has only been run on the author's X4 Pro; the Register screen across a
  boundary wake, the poll wake, Method step timers with Register, "Reset the clock check", join-by-profile and the
  rest of [DEVICE-TEST.md](DEVICE-TEST.md) are open.
- **The X4 Classic** has never run on real Classic hardware. `partitions-x4c.csv` comes from the SDK's documentation,
  not from a reader. See [X4C-WIFI-DEVICE-ACCEPTANCE.md](X4C-WIFI-DEVICE-ACCEPTANCE.md) for the Classic-only items
  (panel image surviving a timer wake, orientation/flip, bezel insets, charge polarity, GPIO4).
- **Real networks.** A university campus network and a hospital network with EAP-TLS have not been tried against.
- The internal clock's drift (2 % is an assumption), and the new clock rules on a real RTC chip.
- Sleep current, polling energy and battery life; heap peaks under TLS and rendering.
- The sender page on an iPhone (camera HEIC, Home Screen install) and a hosted HTTPS deployment.

## Known limits

- No username-and-password enterprise Wi-Fi and no captive-portal login, on purpose.
- The EAP supplicant cannot judge the server certificate's dates in this build.
- File Transfer, OPDS, KOReader Sync and the clock-sync screen join through the saved networks only, not through the
  university profile.
- Keys, tokens and the profile are stored unencrypted; see [../SECURITY.md](../SECURITY.md).
- Firmware images built from this repository are GPL combined works; see [../NOTICE](../NOTICE). They are not attached
  to releases.

## Later ideas (not started)

Message history, quiet hours, multiple readers per mailbox, weather and agenda providers, direct reader-to-reader
sending.
