# Security

Plaque OS is beta firmware, tested on a single reader. It has had no independent security audit.

## Reporting a vulnerability

Please report a vulnerability **privately**, with GitHub's private vulnerability reporting on this repository
(Security > Report a vulnerability, https://github.com/samporter14/plaque-os/security/advisories/new). Do not open a
public issue for it. Say what you found, how to reproduce it, which firmware line (Settings > About) you ran, and what
you think the impact is. This is a one-person project: expect an answer in days, not hours, and no bounty.

## What is in scope

The firmware in this repository: the mailbox client (bearer token handling, TLS and certificate checks, response
parsing), the university Wi-Fi profile store and join code, the web server (File Transfer, WebDAV, the settings API),
the USB setup link, and the SD-card firmware update path. The demo mailbox in `sender-web/` is a development tool, not
a hardened service.

Out of scope: the CrossPoint Reader base code that is unchanged (report it to
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)), the FreeInk SDK and the third-party
libraries (report them upstream), and anything that needs physical access to a reader (see below).

## Known limits, by design

- NVS and flash encryption are off and no eFuse is burned. Tokens, a Wi-Fi profile and an EAP-TLS client key are stored
  unencrypted on the reader (flash) and on its SD card. Anyone with physical access can read them. Use revocable,
  short-lived credentials.
- The EAP supplicant cannot judge the server certificate's dates in this build; the CA chain and server name are
  required.
- Time comes from NTP, which is unauthenticated; the clock check limits the damage but cannot authenticate a time.
  See `docs/SPEC-UNIVERSITY-WIFI.md` section 7 and `docs/message-tls-candidate.md`.

## Supported versions

Only the latest commit on `main`.
