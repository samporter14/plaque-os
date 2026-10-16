# X4 Classic support and university Wi-Fi: the contract

The contract between the firmware (Plaque OS) and the Plaque Mac app
(github.com/samporter14/plaque). Both sides build against this file; nobody invents a second
version of a format defined here. The same file is kept in both repositories.

## Ground rules

- X4 Pro keeps working exactly as today: touch, keys, frontlight alerts, Method, Notes,
  Almanac, scheduled downloads, sleep/resume, several readers.
- Existing reader configuration (`/.crosspoint/ambient-config.json`, the saved Wi-Fi store),
  mailbox routes, authentication and book formats are unchanged. Old readers keep working
  with no reconfiguration. **No Worker change** is needed for this work: everything new is
  local (card, USB, reader screen).
- Internal identifiers stay `lamina` where compatibility needs them (`/Lamina/` on the card,
  the `lamina` NVS namespace, `X-Lamina-*` headers). User-facing text says Plaque.
- Never request or store a user's university account password. No PEAP/TTLS
  username-password methods are offered at all. A captive-portal network gets an
  "unsupported" explanation, never a login form on the reader.
- Never log or export passwords, PSKs, tokens, private keys or book contents.
- The tests never deploy, flash, write real SD cards, call production with real
  credentials, change device partitions or burn eFuses. They use synthetic data;
  synthetic private keys are generated at test time, never committed.

## 1. Board capabilities (firmware)

A small capability layer separates *what the board is* from *what Plaque offers on it*.

| | X4 Pro (`x4pro`) | X4 Classic (`x4c`) |
|---|---|---|
| Panel | 480×800 portrait, 1-bit | same |
| Touch, Home key | yes | **no** |
| Frontlight | yes | **no** |
| Keys | side page keys, power, capacitive Home | side page keys, power, four bottom keys (Left, Right, Confirm, Back) |
| Method, Notes, Almanac, Register, mailbox, identity | yes | yes |
| Scheduled downloads, timed wake, step timers | yes | yes |
| Step-timer alert | inverted box, frontlight pulse, stays awake until a press | inverted box (monochrome only), stays awake until a key press |
| USB Drive (MSC), USB serial | yes | yes (verify) |
| Wi-Fi bands | 2.4 GHz only (ESP32-S3) | same |

The renderer and the timer-box coordinate contract (480×800 page pixels) are unchanged.
Classic never shows touch or frontlight controls and never mentions light, sound or vibration.

The network features below are offered by capability, not by model, and no capability token was added or changed: the
Network screen, the installed profile and `/Lamina/network-profile.json` need at least one of `wifi_open`, `wifi_psk`,
`wifi_eap_tls` (a profile of a kind the board lacks is refused as `auth_unsupported`); Import from card needs `wifi_open`
or `wifi_psk` (a key never travels on the card, and there is no separate card-import token); Connect to Mac needs
`usb_setup` as well as a profile; Check connection needs the mailbox. Both Plaque boards have all of them; any other board
has none of these features, whatever its Wi-Fi hardware.

Register (the sleep screen that shows an instrument's schedule, `SPEC-REGISTER.md` in the Plaque repository) is offered
wherever Notes is: a board that has the Register bit, the mailbox, timed wake and a sleep screen (`offersRegister`). It
adds one token, `register` (section 2), after all the others. Its checks and scheduled polls go through the same joins as
every other mailbox caller (section 6).

## 2. `/Lamina/reader.json` (schema 2, written by the reader)

Written at **every boot on both boards**, independent of the network (only rewritten when
the content differs). Old fields keep their meaning; new fields are optional to readers of
the file. A file with only `reader_uuid` and `firmware` (schema 1) stays valid.

```json
{
  "reader_uuid": "0b6c9f9e-3c1a-4c7e-9f00-1d2e3f4a5b6c",
  "firmware": "1.6.5-x4c+2a6befc",
  "schema": 2,
  "model": "x4c",
  "model_name": "Xteink X4 Classic",
  "station_mac": "AA:BB:CC:DD:EE:FF",
  "hotspot_mac": "AA:BB:CC:DD:EE:F0",
  "wifi_bands": ["2.4GHz"],
  "capabilities": ["keys", "method", "notes", "almanac", "step_timers", "timed_wake",
                   "scheduled_downloads", "usb_drive", "usb_setup",
                   "wifi_open", "wifi_psk", "wifi_eap_tls", "register"]
}
```

- `model`: `x4pro` or `x4c`. Unknown values mean "an unknown Plaque reader".
- `station_mac`: `esp_read_mac(ESP_MAC_WIFI_STA)`, uppercase, colon-separated. **This is the
  address Wi-Fi registration needs.** `hotspot_mac` (`ESP_MAC_WIFI_SOFTAP`) is the reader's
  own File Transfer hotspot address and must never be registered; the Mac labels it so.
- `capabilities`: tokens `[a-z0-9_]`, unknown ones ignored. `touch` and `frontlight` appear
  only on the Pro. `register` is additive: it is listed last, after `wifi_eap_tls`, so that every earlier token keeps its
  name and its place, and a reader of the file that does not know it (an older Mac) ignores it like any unknown token.
  Its meaning: the reader can show an instrument's schedule as its sleep screen. A firmware that predates it does not
  list it; a reader of the file must not read its absence as a fault.
- Readers of the file (the Mac) treat any missing field as unknown, never as an error.

## 3. Network profile bundle (Mac → reader)

One JSON object, at most 16 KB, UTF-8:

```json
{
  "kind": "plaque-network-profile",
  "schema": 1,
  "profile_id": "5f2c9a10",
  "name": "Lab 100 Wi-Fi",
  "institution": "umich-its",
  "category": "registered-device",
  "ssid": "MSetup",
  "hidden": false,
  "auth": "open",
  "psk": null,
  "eap": null,
  "time_servers": [],
  "use_dhcp_time": false,
  "created_at": 1792000000
}
```

| Field | Rule |
|---|---|
| `kind` | exactly `plaque-network-profile` |
| `schema` | exactly `1`; anything else is refused |
| `profile_id` | 8 lowercase hex, chosen by the Mac |
| `name` | 1–40 chars, no `Cc`/`Cf`/`Zl`/`Zp` characters; display only |
| `institution` | `umich-its`, `michigan-medicine-hits` or `other` (informational) |
| `category` | `registered-device`, `shared-password`, `device-credential`, `enterprise-certificate` (informational, for display and export) |
| `ssid` | 1–32 bytes UTF-8, no control characters |
| `hidden` | bool, optional (default false) |
| `auth` | `open`, `psk` or `eap-tls` |
| `psk` | `psk` only: 8–63 printable ASCII, or exactly 64 hex digits |
| `eap` | `eap-tls` only, see below |
| `time_servers` | optional, 0–3 entries, each a hostname (LDH labels, ≤ 63 chars) or an IPv4 literal; empty means the reader's defaults (`pool.ntp.org`, `time.google.com`, `time.cloudflare.com`) |
| `use_dhcp_time` | optional bool: also accept NTP servers offered by DHCP (lwIP option) |
| `created_at` | Mac clock, informational. **Never used as trusted time.** |

Unknown keys are refused (`bad_field`). `psk` must be null/absent unless `auth` is `psk`;
`eap` must be null/absent unless `auth` is `eap-tls`.

`eap` (EAP-TLS only; no other EAP methods exist):

```json
{
  "identity": "plaque-0b6c9f9e@example.edu",
  "server_domain": "radius.example.edu",
  "ca_pem": "-----BEGIN CERTIFICATE-----\n...",
  "client_cert_pem": "-----BEGIN CERTIFICATE-----\n...",
  "client_key_pem": "-----BEGIN PRIVATE KEY-----\n..."
}
```

- `identity`: 1–127 printable ASCII, supplied by IT. (The contract first said 128; the reader's
  `esp_eap_client_set_identity()` takes 1–127 bytes, so the reader refuses 128 and the Mac follows it.)
- `server_domain`: required, 1–253 chars, hostname syntax. The authentication server's
  certificate must chain to `ca_pem` **and** match this name.
- `ca_pem`: 1–3 PEM certificates, each a CA certificate (basicConstraints CA:TRUE).
- `client_cert_pem`: exactly one PEM certificate.
- `client_key_pem`: unencrypted PKCS#8, PKCS#1 RSA (2048–4096 bits; the supplicant
  takes keys of at most 4096 bytes) or SEC1 EC (P-256/P-384).
  It must match the public key of `client_cert_pem`. Encrypted keys are refused
  (`key_encrypted`); the Mac decrypts a PKCS#12 or encrypted key in memory first.
- If trusted time is available, an expired or not-yet-valid client certificate is refused
  (`client_cert_expired`, `client_cert_not_yet_valid`); without trusted time the dates are
  shown, not judged.

### Error codes (shared by reader replies, import results and the Mac's own validation)

`too_large`, `bad_json`, `bad_kind`, `bad_schema`, `bad_field`, `name_invalid`,
`ssid_invalid`, `auth_unsupported`, `psk_invalid`, `eap_identity_invalid`,
`server_domain_invalid`, `ca_invalid`, `client_cert_invalid`, `client_key_invalid`,
`key_encrypted`, `key_mismatch`, `client_cert_expired`, `client_cert_not_yet_valid`,
`time_server_invalid`, `card_key_refused`, `checksum`, `declined`, `storage_full`,
`storage_failed`, `busy`.

## 4. Where the reader keeps a profile

- One installed profile at a time, in internal flash: NVS namespace `plaque_net`. **Not** in
  the SD card's XOR-obfuscated Wi-Fi store, which keeps its saved networks unchanged.
- Replacement is all-or-nothing: the new bundle is validated completely in RAM, written
  beside the old one, and only then made active; the old one is erased last. Any failure
  (including NVS running out of space) leaves the working profile as it was.
- A profile may take at most **8192 bytes** (`MAX_STORED_BYTES`) of that storage, counted as the
  reader stores it: a description (96 bytes plus the name, SSID, identity, server name, certificate
  subject text, time servers and 32 bytes for each CA's digest), the passphrase, and the CA
  certificates, client certificate and key as DER (about three quarters of their PEM). Over it the
  reader answers `storage_full`. The Mac counts the same bytes before sending and says so (an RSA-4096
  client certificate with three CAs can reach it). The subject text is Mbed TLS's `mbedtls_x509_dn_gets`
  form (`CN=…, O=…`), cut to 200 characters.
- Removal deletes the profile's entries (the PSK, the CA certificates, the client certificate and key) and leaves saved
  networks alone. "Deletes" is what `nvs_erase_key` does: it is **logical**. The entry is marked deleted and every API of
  the reader stops returning it, but the bytes stay in the flash page until NVS reclaims that page, and flash encryption
  is off, so a removed key is not scrubbed from the chip. Replacing a profile deletes the old one the same way, and the
  reader says so before it does (see section 5).
- Storage protection, stated plainly: NVS is in the reader's internal flash, not on the
  removable card, but NVS encryption and flash encryption are **off** (turning them on burns
  eFuses irreversibly, which this work does not do). Anyone with physical access and a USB
  cable can read the flash with standard tools, and that includes keys and passwords that
  were removed or replaced and not yet overwritten (see Removal above). A per-device
  certificate should be one IT can revoke.
- Buffers that held secrets are zeroed after use (`mbedtls_platform_zeroize`).

`/Lamina/network-profile.json` (no secrets; rewritten when the profile changes and at boot
when it differs):

```json
{"schema": 1, "installed": true, "profile_id": "5f2c9a10", "name": "Lab 100 Wi-Fi",
 "institution": "umich-its", "category": "registered-device", "ssid": "MSetup",
 "auth": "open", "time_servers": [], "use_dhcp_time": false,
 "server_domain": null, "ca_sha256": [], "client_cert_subject": null,
 "client_cert_not_after": null, "installed_via": "usb"}
```

With no profile: `{"schema": 1, "installed": false}`.

## 5. Getting a profile onto the reader

Two transports. Both end in the same validation and the same storage.

### USB (serial), required for `eap-tls`

The reader's normal USB personality is USB Serial/JTAG (CDC). The Mac chooses a port for the
person only when it is the one Espressif USB device (vendor 0x303A); any other port is the person's
explicit choice. It opens the port exclusively (`TIOCEXCL`) and says "Another program is using the
reader's USB port. Close it and try again." if another program has it. The reader listens **only**
while it shows **Settings › System › Network › Connect to Mac**. Line protocol, ASCII,
`\n`-terminated; every protocol line starts with `PLAQUE-NET/1 ` and the Mac ignores every
other line (log output shares the port).

| Mac sends | Reader answers |
|---|---|
| `PLAQUE-NET/1 HELLO` | `PLAQUE-NET/1 READY <max_bytes>` |
| `PLAQUE-NET/1 INFO` | `PLAQUE-NET/1 INFO <base64 of reader.json content>` |
| `PLAQUE-NET/1 PROFILE` | `PLAQUE-NET/1 PROFILE <base64 of network-profile.json content>` |
| `PLAQUE-NET/1 RESULT` | `PLAQUE-NET/1 RESULT <base64 of connection-check.json>` or `PLAQUE-NET/1 RESULT none` |
| `PLAQUE-NET/1 PUT <bytes> <sha256-hex>` then `PLAQUE-NET/1 DATA <base64, ≤ 256 chars>` lines, then `PLAQUE-NET/1 END` | `PLAQUE-NET/1 CONFIRM` while the reader asks the person to approve on screen, then `PLAQUE-NET/1 OK installed` or `PLAQUE-NET/1 ERR <code>` |
| `PLAQUE-NET/1 REMOVE` | asks on screen, then `PLAQUE-NET/1 OK removed` or `PLAQUE-NET/1 ERR declined` |

- The reader shows the profile's name, SSID, auth, server name and client certificate
  subject/expiry, and installs only after the person chooses **Install** (keys or touch).
  Back declines (`ERR declined`). The question also says, briefly, what the reader cannot do
  for the person and how the secret is kept: for `eap-tls`, that the server certificate's
  dates are not checked and that the key is stored unencrypted in the reader; for `psk`, that
  the password is. When a profile is installed already it says what installing replaces
  ("Replaces <name>, <kind>."), and when that is a certificate profile, that its key is
  deleted from the reader. The SSID is drawn without invisible and direction-changing
  characters (Unicode Cf, Zl, Zp become U+FFFD); the stored SSID and its validation are
  unchanged.
- The reader never echoes payload bytes and logs only sizes and result codes.
- A transfer that stalls for 10 s is dropped (`ERR busy` on the next line).
- While the question is up, every protocol line is answered `ERR busy` (the Mac says: "The reader is still asking
  about an earlier profile. Choose Back on it, then try again."). The question does not wait for ever. After **2 minutes**
  with no answer (each change of the highlighted choice, by key or touch, starts the time again) the reader takes it as Back: `ERR declined`
  is sent, the dialog goes, the validated profile (and a certificate profile's key) is forgotten, and the screen says
  plainly "Nobody answered, so nothing was changed. Send it again from the Mac." If the USB cable has been out for
  3 seconds, the question is dropped the same way without a reply (nobody to hear it) and the screen says "The Mac is no
  longer connected, so the question was dropped and nothing was changed." A transfer in progress is dropped and its bytes
  wiped when the link is gone for 3 seconds. Install, Remove and Back still work exactly as before until then; after the
  question has gone, installing is no longer possible, so a Mac that stopped waiting cannot have a profile installed under
  it later.
- No pacing is needed. The reader's USB Serial/JTAG driver never makes the host wait: it
  empties its FIFO into a receive queue and discards what does not fit. While Connect to Mac is
  open the reader therefore sizes that queue for a whole legal transfer (the PUT line, 86 DATA
  lines of 256 characters and END, 24 KB in all, CRLF allowed), so a sender may write the
  lines back to back, whatever the reader's screen is doing. If the memory is not there the
  queue is smaller (8 KB, then the default), which the reader logs; a byte lost then fails the
  transfer's checksum (`ERR checksum`), and the Mac sends it again.

### Card inbox, for `open` and `psk` only

- The Mac writes `/Lamina/Provision/network-profile.json` (USB Drive mode or a card reader). It
  writes a **passphrase** only to a card whose own `/Lamina/reader.json` is schema 2 or later and lists
  `wifi_psk` (the reader has no separate card-import capability; its Network screen ships with the
  schema 2 network tokens). Otherwise the passphrase would sit on the card, unread and unerased, and the
  Mac says to use USB. An open profile holds no secret, so it is not refused, but the person is asked first
  when the card doesn't say the reader imports. Erase from Card also removes a leftover
  `.network-profile.json.tmp` from a write that was cut short. The Mac never follows a link on a card:
  every access is by descriptors opened one step at a time without following links, and a link where a
  folder or file should be (`Lamina`, `Lamina/Provision`, the inbox or its scratch file) is refused with a
  message, touching nothing behind it. After a write the Mac says the order: Import from card, the reader
  reads and erases the file, shows the profile, and installs it only after Install (Back declines).
- The reader reads it only when the person picks **Import from card** on the Network screen.
- A card bundle containing an `eap` object or a private key is refused (`card_key_refused`);
  private keys never travel on the card.
- After reading, whatever the result, the reader overwrites the file with zeros and deletes it.
- A bundle that validates is not installed at once. The reader shows the same question as for
  USB (name, SSID, security, the stored-unencrypted note, and what it replaces, with the key
  warning for a certificate profile) and installs only after **Install**. **Back**, the
  highlighted choice, or leaving the screen declines: nothing is installed, the installed
  profile is untouched, and the result is `rejected` with `error` `declined`. The inbox file
  is already gone either way.
- It writes `/Lamina/network-import.json`:
  `{"schema": 1, "at": <trusted unix or null>, "result": "pending"|"installed"|"rejected", "error": <code or null>, "profile": {"name","ssid","auth"} or null}`.
  The file says `"pending"` (`error` null, the profile's name, SSID and auth) from the moment the bundle has been read until
  the person answers, so that an earlier `installed` cannot be read as this import's result while the question is up or if
  the reader is switched off before it is answered. A reader that is switched off with the question up leaves `pending`
  and installs nothing. A reader that reads `pending` means "not decided yet"; a result word a reader does not know is
  no result.

## 6. Joining: one path for every caller

Manual checks (Method shelf, Messages, the register), scheduled wakes (Notes/Almanac/Register/Stage B), the
sleep-event delivery before deep sleep and the connection check all join through the same code:

1. If a profile is installed, join it (open, PSK or EAP-TLS with CA, server name and client
   certificate).
2. Otherwise, or if the profile's join fails, the saved networks as today (manual: last
   used first, then a scan of the saved networks in range; scheduled wake and sleep-event
   delivery: last used only, never a scan).

Wi-Fi goes off at the end of every path, as today (and an EAP-TLS join's supplicant state is
cleared before it does).

- **An EAP-TLS join only takes an enterprise network.** The station's scan threshold is set
  to WPA2-Enterprise before it connects, so an open or PSK network of the same name is never a
  candidate, whatever its signal. (A WPA3-PSK network of that name ranks above WPA2-Enterprise in
  the driver's order and is still caught after the association.)
- **A wake's join gives each candidate a budget of its own, and tries first the one that
  joined last.** Who joined last is kept in RTC memory (it survives deep sleep, not a
  power-off) and is updated by every profile join, wake join and shelf check; unknown means
  the profile first. The budgets: a saved network 10 s, an open or PSK profile 10 s, an
  EAP-TLS profile 25 s (scan, handshake, 4-way, lease), so the worst case is 20 s with a
  profile and 35 s with an EAP-TLS one. With no profile the wake is the last-used network
  alone with 10 s, exactly as before. The poll's total budget (join, note, books) grows with
  it, 150 s without a profile, 160 s or 175 s with one, and a poll that would run into a
  running step timer's end is skipped. A Register poll asks the mailbox once where a note
  poll asks twice, so the same budget covers it; a Register boundary wake does no network at
  all, but is skipped by the same rule (it is an ambient timer wake like the poll), so a boundary
  in the last 150 s (160 s, 175 s) of a step timer waits for the timer.
- **Where the profile does not reach.** Only the code above joins through the profile.
  Interactive features that bring Wi-Fi up themselves (File Transfer, OPDS, KOReader Sync, the
  clock-sync screen) join through the Wi-Fi screen and the saved networks only: a network that
  exists only as a profile (an EAP-TLS network, say) cannot be used by them. Rebuilding them is
  outside this work; the limit is deliberate and stated here and in `x4c-support.md`.

## 7. Time

- `time_servers` from the profile replace the default SNTP servers while the profile is
  installed; with no profile, or an empty list, the defaults stay exactly as today. They are
  applied inside the clock library: `trustedtime::startSync()`, `beginSync()` and `syncNow()` run a prepare hook that
  main() installs (`netprofile::applyTimeSettings`) just before SNTP is configured, so every caller of a sync gets them:
  the Notes poll, the Method shelf, Check connection, the Register poll, the wake path's `startSync()`, the Wi-Fi
  setup's sync and Settings › Clock › Sync clock now (`HalClock::syncFromNTP` syncs through the same calls). The
  same list given again changes nothing, not even a running client. `use_dhcp_time` is switched on before the join,
  because the DHCP option arrives with the lease; that stays in the joins.
- Certificate chain, host name and date validation are never disabled for HTTPS.

### Clock provenance

| Source | Meaning | Trusted for certificate dates |
|---|---|---|
| `sntp` | an SNTP answer received this session, used within 90 s (today's rule) | yes |
| `retained` | the clock has run continuously since an SNTP sync recorded in NVS and RTC memory: a deep-sleep wake (not a reset or power loss) whose RTC-memory sync record matches NVS, or a battery-backed RTC chip with its integrity flag clear that was set from that sync | yes, within the bounds below |
| `none` | anything else, including the persisted floor after a reset | no |

- Error bound: `2 s + age × drift`, with `drift` = 50 ppm for a crystal RTC chip and
  20 000 ppm (2 %) for the ESP32-S3 internal slow clock. **The 2 % is an assumption**, not a
  measurement: the internal oscillator is a 136 kHz RC whose real drift varies with
  temperature and the unit, and the acceptance checklist has the reader measure it (record a
  sync, sleep for an hour and for eight, compare with SNTP). Retained time is trusted only
  while the bound is ≤ 10 minutes and the age is ≤ 7 days. The internal clock's reach is therefore about 8.3 hours at
  2 % (the bound is 2 s + age × 2 %, which reaches 600 s at 29 900 s of age), not 30 minutes; the chip's is the 7 days.
- **Every sync is judged before it is believed** (`clockprov::judgeSync`). All paths that set the clock go through the clock
  record: message_tls (the Notes poll, Register, Check connection, the Method shelf) and `HalClock::syncFromNTP`
  (Settings › Clock › Sync clock now, the first Wi-Fi join), which no longer writes the RTC chip itself. The chip is the
  witness: it is set only from a judged sync, counts on its own crystal, and the network cannot change it. A sync is
  - **refused** when it is not a time (before 2025 or at or past 2100), or behind the proven floor by more than 10 minutes
    (a backward-spoofed reply); nothing is recorded, the RTC chip is not written, and the log says so as an error. The one
    exception is the **first sync of a reader with no clock record** (the reader that has just replaced an earlier
    firmware, or whose record was cleared): that firmware persisted the raw internal clock at every sleep, which runs up
    to 2 % fast, so the floor it left can be minutes or hours ahead of true time and would refuse every honest sync until
    real time passed it. Such a first sync may be behind the floor by up to a day, if the RTC chip (when it answers)
    agrees with it within 10 minutes; it is recorded, and the persisted, proven and monotonic floors are **lowered** to it,
    once. With a record, nothing is forgiven: a sync behind the floor by more than 10 minutes is refused, however small the
    step. A floor more than a day ahead is the person's to clear (Reset the clock check);
  - **recorded and confirmed** when the chip, read before it is written, agrees with it within 10 minutes plus the crystal's
    drift since the chip was set: the record, the RTC chip and the persisted floor follow it;
  - **used for the session only** when the chip contradicts it and the chip's time had itself been confirmed (a forward
    spoof of a day or three months on a reader that has synced before, or the chip set back): the system clock keeps what
    SNTP set for this session (HTTPS in this session rests on it as before), and nothing is recorded or persisted, so the
    lie does not outlive the boot;
  - **recorded, unconfirmed** when nothing can witness it (the first sync ever, a chip that lost its battery or was written
    by something else) or when the chip contradicts a sync nobody confirmed: the record and the chip are set from it, so
    that the chip can witness the next sync, but the persisted floor moves by **at most one minute**. (A rule that
    persisted the clock, or capped each sync, would be wrong either way: a reader that was off for months legitimately
    advances by months, and a forward lie persisted would shut the reader out of HTTPS until true time caught up.) The next
    sync the chip agrees with confirms the chain and the floor catches up.
  The chip is written with the sync's own second, never the floor.
- **The proven floor.** The persisted floor in NVS, raised only by a confirmed sync, by the chip's own time less its error
  bound (at sleep entry and boot, only for a confirmed record), and by the one-minute step above. It never includes the
  system clock, which the internal oscillator runs up to 2 % fast through a sleep. The retained time is held to it: a
  retained time behind it is raised to it, and one behind it by more than 10 minutes cannot be right and is not trusted
  (each source, the internal timer and the chip, is held to this, and the other may still vouch). The monotonic floor under
  TLS dates (`trustedNow()`, never earlier than a time seen this boot) is unchanged; a time at or past 2100 never
  counts toward either.
- **Reset the clock check** (Settings › System › Clock; touch or keys; asks first, **Keep** highlighted): "Use this if the
  reader shows a date in the future and can't connect." It clears the persisted floor, the RAM floors and the sync record
  (NVS and RTC memory), withdraws the permission a sync gave HTTPS and EAP-TLS (the 90 seconds, and the retained one), and
  sets the system clock to zero, so that the reader reads as having no time at all (not the earliest plausible second,
  which `trustedNow()` would believe); the chip is left alone and, with no record, vouches for nothing. The row is offered
  on every board that has the mailbox or a network profile, whether or not the RTC chip answered its probe. The next sync starts the chain again. This is the way out of a floor an earlier firmware persisted
  from a spoofed reply, of a confirmed chip that was set wrongly, and of a forward lie at the first sync.
- **Residual risks, stated.** SNTP is unauthenticated, as it always was. A forward lie at the very first sync is recorded
  (so the chip holds it until the next sync replaces it, because nobody had confirmed it) and costs a lockout of at most a
  minute of floor; the retained clock claims the lie until that next sync. A lie that is consistent across two syncs (a
  hostile network on both) confirms itself; the reset clears the floor it persisted. A confirmed chip that is genuinely
  wrong (set by hand, a battery swap that kept the flag) makes every later sync "session only" until Reset the clock check
  or the chip loses power. A DHCP-supplied NTP server (`use_dhcp_time`) is one more source of lies and is judged like any
  other. A board without a working chip never persists more than the one-minute step and has no retained clock from a
  reset.
- With retained time, a certificate is valid only if `[now − bound, now + bound]` lies inside
  its validity period.
- HTTPS prefers a fresh SNTP sync; if SNTP fails and retained time is trusted, HTTPS
  proceeds on retained time with the widened check and records `clock: "retained"`;
  otherwise it stops with "Couldn't synchronize the clock." and the reader stays offline.
  This holds for every caller, the Method shelf included, so on a network that blocks NTP the
  shelf and the connection check agree.
- EAP-TLS: the reader always checks that the server's certificate chains to the profile's CA and
  matches `server_domain`. The date check on the server certificate is **not performed** by the
  supplicant in this firmware build (ESP-IDF's mbedTLS is built without `MBEDTLS_HAVE_TIME_DATE`),
  whatever the clock says, so the result reports `eap_time_checked: false`. The client certificate's
  dates are judged on import only when trusted time exists. `certificateClockTrusted()` still gates
  the supplicant's own time-check flag for a future build that can judge dates.
- Mailbox timestamps, the Mac's `created_at` and file times never set or vouch for the clock.
- No "time from the Mac" path exists.

## 8. Check connection (runs on the reader)

**Settings › System › Network › Check connection.** Every stage has a deadline, Back cancels
at any point within 100 ms of input polling (the TLS handshake may hold it up to its 5 s
socket bound, as Method's check does), and on every exit Wi-Fi goes off, power-save guards
drop and buffers are freed. A running protocol page, step timer and downloaded books are
untouched; a due step-timer alert takes precedence and cancels the check.

| # | Stage id | Deadline | Done when |
|---|---|---|---|
| 1 | `join` | 30 s (profile EAP-TLS), 20 s otherwise | associated and authenticated to the configured network |
| 2 | `network` | 10 s | an IPv4 address, gateway and DNS server |
| 3 | `dns` | 8 s | the mailbox host resolves |
| 4 | `time` | 20 s | trusted time (`sntp`, or `retained` per section 7) |
| 5 | `https` | 15 s | TLS to the mailbox host with a verified chain, name and dates |
| 6 | `mailbox` | 10 s | `GET /api/device/<id>/books` with the reader's token answers 200 with a valid list |

Redirects are never followed; a 3xx is reported as `redirect` with the `Location` host as an
observed fact. No unrelated public site is used as a success test.

Each stage ends `ok`, `failed`, `canceled` or `not_run`. Plain wording on the reader:

| Outcome | Reader says |
|---|---|
| join failed | Couldn't join this network. |
| network failed | Wi-Fi joined. Couldn't get a network address. |
| dns failed | Wi-Fi joined. Couldn't resolve the mailbox. |
| time failed | Couldn't synchronize the clock. |
| https failed, certificate | Couldn't verify the server certificate. |
| https failed, other | Couldn't reach the mailbox. |
| mailbox 401/403 | Mailbox access was rejected. |
| mailbox other | The mailbox answered, but not as expected. |
| all ok | Connected to the mailbox. |
| canceled | Check canceled. |
| not set up | The mailbox isn't set up. Add it under Settings › System › Messages. |

Failures are reported as observed facts ("the reader did not see this network name",
"the server answered with a redirect to portal.example.edu"). The reader never labels a
failure "captive portal" or "registration expired".

### `/Lamina/connection-check.json` (schema 1)

```json
{
  "schema": 1,
  "kind": "plaque-connection-check",
  "finished_at": 1792000123,
  "clock": "sntp",
  "boot_ms": 48211,
  "reader_uuid": "...", "model": "x4c", "firmware": "...", "station_mac": "AA:BB:CC:DD:EE:FF",
  "network": {"source": "profile", "name": "Lab 100 Wi-Fi", "ssid": "MSetup", "auth": "open"},
  "mailbox_host": "mailbox.example.workers.dev",
  "result": "failed",
  "message": "Wi-Fi joined. Couldn't resolve the mailbox.",
  "stages": [
    {"id": "join", "status": "ok", "ms": 4210, "rssi": -61, "channel": 6, "eap_time_checked": null},
    {"id": "network", "status": "ok", "ms": 820, "ip": "10.1.2.3", "gateway": "10.1.0.1", "dns": ["10.0.0.53"]},
    {"id": "dns", "status": "failed", "ms": 8000, "detail": "timeout"},
    {"id": "time", "status": "not_run"},
    {"id": "https", "status": "not_run"},
    {"id": "mailbox", "status": "not_run"}
  ]
}
```

- `finished_at` is null when the reader had no trusted time; the Mac then says
  "time unknown (the reader's clock wasn't set)" and never substitutes its own clock.
- `detail` codes: join `nothing_configured`, `not_found`, `auth_failed`, `timeout`,
  `failed` (plus numeric `reason` when the driver gave one); network `no_address`,
  `timeout`; dns `failed`, `timeout`; time `failed`, `timeout`; https `connect_failed`,
  `certificate`, `tls_failed`, `timeout`, `redirect` (+ `location_host`); mailbox
  `unauthorized`, `not_found`, `server_error`, `unexpected_response`, `timeout`
  (+ `http_status`).
- No tokens, passwords, keys, book titles or note contents. `network` is omitted when nothing
  is configured. Written through `HalStorage`, never while USB Drive owns the card.

## 9. The Mac's university setup (Settings › Readers › Set Up University Wi-Fi…)

Two institutions, one shared network-profile and diagnostic model:

- **U-M campus — ITS.** Registered-device networks (MSetup), personal or shared password
  networks, IT-issued per-device credentials (a per-device passphrase or certificate), and
  certificate (EAP-TLS) profiles where IT provides one.
  - MSetup's published IoT instructions apply to Ann Arbor and Dearborn campus buildings
    outside Michigan Medicine areas.
  - Register at the U-M WiFi Configuration and Registration site
    (`https://msetup.its.umich.edu/`, choose "Other Device", enter the station MAC), then
    join MSetup. The Mac copies the MAC and opens the page; the person signs in there.
  - That process is for personal devices. University-owned or managed devices (MiWorkspace,
    Coreimage and other managed devices) do not use self-registration: contact unit IT or
    the ITS Service Center.
  - The U-M Network Security Standard (DS-14) says MGuest and MSetup should not be used for
    sensitive institutional data, and to use HTTPS in addition. The Mac says that, no more: if
    protocols, notes or tables carry sensitive data, use an institution-approved secure profile
    instead. It says nothing about whether HTTPS makes these networks suitable, so neither does the Mac.
    The note comes before the steps, on both institutions' tabs.
  - MGuest needs a captive portal that the reader can't complete: unsupported. The Mac warns when
    the network name starts with "MGuest" (any case) and the profile is open or registered-device.
- **Michigan Medicine — HITS.** IT-assisted. Export the reader information for HITS, enter
  the network details or profile HITS supplies, send it to the reader, then run the same
  reader-side check. No SSID is pre-filled, public self-registration is not assumed, and
  nothing claims HITS approval. A guest network that asks for a sign-in or terms in a browser is
  unsupported there too (named only as such, with no network name). HITS guidance: KB0018780 "How to have a device Managed by
  Michigan Medicine" (sign-in required); HITS Service Desk 734-936-8000.
- Every path shows: the reader uses **2.4 GHz only**; a 5 GHz-only network won't work, and
  not seeing a network name proves nothing about why.

## 10. IT information export (Mac only, never sent automatically)

Reviewable text, saved or copied by the person: reader name, model, UUID, station MAC,
firmware; 2.4 GHz requirement and configured authentication method; mailbox host name and
the outbound services needed (DNS to the network's resolvers, NTP UDP 123 to the configured
or default time servers, HTTPS TCP 443 to the mailbox host); the latest check results with
when they were obtained; optional building/room and contact details typed by the person.
Normal operation is outbound HTTPS polling of the mailbox. Manually started services
(File Transfer's web server and its `Plaque-XXXX` hotspot, USB Drive, the USB setup link)
are listed as such. No passwords, PSKs, tokens, private keys, book or sample contents.
