# Desk Notes local demo

This folder contains the responsive sender PWA, a Python/SQLite mailbox, server-side image preparation,
and a desktop polling client. The desktop client can exercise the actual firmware metadata parser.

From the repository root:

```sh
python3.12 -m venv .venv
.venv/bin/python -m pip install -r sender-web/requirements.txt
cd sender-web
../.venv/bin/python setup_local.py
../.venv/bin/python app.py
```

Open **http://localhost:8787**. Copy `SENDER_SECRET` from the new local `.env` file into the login field.
Choose Text, Draw or Photo, prepare a preview, then Send. The exact prepared image is published.
On Safari on iPhone, the PWA can be added to the Home Screen once hosted at your HTTPS origin.
The default service accepts sender requests only at its configured `PUBLIC_ORIGIN`.

In another terminal, from `sender-web/`:

```sh
../.venv/bin/python demo_device.py --once
```

The output is `updated`, `unchanged`, `empty` or `failed`. New content appears in `.data/demo-device/frame.png`.
Repeat the command with no new send: it fetches only metadata and does not rewrite that frame. Stop
the server and repeat: it reports `failed` and keeps the previous frame. For an accelerated desktop
demo, omit `--once`; its default loop checks every 10 seconds. Firmware still defaults to 60 minutes.

To include the actual C++ parser, first build the X4 Pro target once (it downloads ArduinoJson) and then the host
validator, using the build steps in the [README](../README.md) and `test/ambient_frame`:

```sh
cmake -S test/ambient_frame -B build/ambient-host -DCMAKE_BUILD_TYPE=Release
cmake --build build/ambient-host
```

Then run, from `sender-web/`:

```sh
../.venv/bin/python demo_device.py --once --validator ../build/ambient-host/MessageProtocolTest
```

The complete local protocol/authentication tests run with:

```sh
AMBIENT_PROTOCOL_CHECK="$PWD/../build/ambient-host/MessageProtocolTest" \
  ../.venv/bin/python -m unittest discover -s tests -v
```

Synthetic 800×480 fixtures are in `../test/ambient_frame/fixtures/`. The mailbox itself is the mock
endpoint for development; it has the same boring HTTP protocol a deployed service will use.
The desktop client exercises content/version decisions and produces a PNG. It does not emulate the reader's
display driver, deep sleep, Wi-Fi power, touch or firmware execution.

## Configuration and privacy

`setup_local.py` makes distinct random device, sender and session secrets. It preserves an existing `.env`.
The real `.env`, SQLite content, desktop frames and `device-config.local.json` are ignored by Git.
The `.env.example` file contains placeholders. Do not use test credentials outside the test suite.

The generated device configuration has sleeping checks and the message sleep screen turned off. Its
example private IP address needs replacement before hardware use. The configuration belongs at
`/.crosspoint/ambient-config.json` on the reader's SD card, or its fields can be entered under
Settings → System → Messages. Save Wi-Fi through CrossPoint's
normal picker. There are no Wi-Fi credentials in this project configuration.

Local sender requests use `localhost` with HTTP. For a later device LAN test, bind the development server
to the selected private interface and point the reader at that private address; the sender page can stay
on the Mac's localhost origin. This has not been tested with physical hardware.

For deployment, set `ALLOW_LOCAL_HTTP=0`, use an HTTPS `PUBLIC_ORIGIN` with a public trusted certificate,
keep the service private behind a TLS reverse proxy, and set fresh secrets. Use a production WSGI server
instead of Flask's development server. The core protocol and SQLite storage have no cloud vendor dependency.
Secure cookies and HTTPS configuration are covered by host tests; actual hosted TLS and ESP32 certificate
validation still require live deployment/hardware testing.

## Known limits

One device and one shared sender secret; no account management or quiet hours. Drafts expire after ten
minutes. The server keeps the latest two versions; device history is not implemented. HEIC conversion
depends on browser support, with JPEG/PNG as portable choices. Text supports the bundled font's glyphs;
use a drawing or photo for emoji or handwriting. Only static app assets are available offline.

Bundled Noto Sans comes from [Google Fonts](https://github.com/google/fonts/tree/main/ofl/notosans) and
uses the SIL Open Font License in `assets/OFL.txt`.
