# Messages TLS date-validation candidate

Status: merged into Plaque OS beta 0.1. Tested on the host only (see "Local results"); nothing here has run on a reader.

## Reason and scope

The installed SDK disables MBEDTLS_HAVE_TIME_DATE. NTP alone or an application macro cannot restore date checks inside its prebuilt verifier. The existing bounded Messages fetch already verifies its public CA bundle and hostname.

The candidate adds date enforcement to that specific transport through the SDK's certificate-verification hook. Existing normal reader/OTA transport, partition layout, bootloader and updater code remain outside this correction.

Evidence and changes:

- `src/network/HttpDownloader.cpp:81`: HTTPS requires a current Messages clock permission before opening the connection.
- `src/network/HttpDownloader.cpp:98`: attach the date-checking wrapper instead of the bare bundle hook.
- `src/ambient/LatestMessageProvider.cpp:80`: obtain a fresh bounded sync before credentials are constructed. Manual `MessageViewActivity.cpp:51` and timer `main.cpp:543` call this same fetch path.
- `src/network/MessageTls.cpp:38`: delegate the original SDK trust verifier first, then add expiry/not-before flags without clearing other verification flags.
- `src/network/MessageTls.cpp:70`: discard prior SNTP completion and reuse CrossPoint's five-second `trustedtime::syncNow`. A restored NVS floor alone never permits a request.
- `src/network/MessageTls.cpp:90`: fail closed on missing, changed or unexpected bundle hooks.
- `src/network/CertificateDates.cpp`: validate UTC calendar fields and inclusive certificate validity boundaries.

The adapter pins Mbed TLS 3.6.6 and checks the installed hook's stateless null-context contract. It reads the version-pinned configuration hook fields, so a future SDK rebase needs review.

## Setup failures must reject

IDF 5.5.5 calls the bundle hook without checking its return value. Merely returning ESP_FAIL would be insufficient after a partially configured bundle.

The candidate therefore requires verification and installs an explicit rejecting callback before setup, then reinstalls it on any setup inconsistency. Tests exercise clock loss, bundle failure, missing verifier, changed verifier and unexpected context.

This behavior is based on the [exact ESP-TLS setup source](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp-tls/esp_tls_mbedtls.c#L885) and the [SDK bundle hook](https://github.com/espressif/esp-idf/blob/v5.5.5/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c#L303).

## Clock policy

Every HTTPS message poll, whether manual or timed, requires a new successful SNTP sync. No cached floor fallback. The synchronized epoch must be between January 2025 and January 2100, and permission lasts at most 90 seconds for metadata/image handshakes.

Clock failure returns the existing Messages failure result. The cached image and panel are preserved by the existing fetch/poll logic. Local development HTTP remains governed by the existing explicit setting.

This adds up to five seconds of network time per check and can fail on Wi-Fi networks that block NTP. Battery cost and real network timing need hardware measurements.

Ordinary SNTP is unauthenticated. An active network attacker can influence clock acquisition; this correction supplies fresh date enforcement, not an authenticated time source. The existing monotonic floor prevents backward steps below previously observed time, but cannot authenticate a new time sample. A signed time source or reviewed accurate RTC policy is a future improvement if that threat needs stronger protection.

## Certificate coverage

The wrapper checks the validity of certificates presented to the chain verifier, including leaf and intermediate certificates. The SDK stores compact trust-anchor names/keys and uses a synthetic placeholder without validity fields. Only the exact pointer identified by `esp_crt_bundle_in_use` is exempt.

A native pointer-identity regression models that documented SDK predicate: the exact placeholder passes while a peer object with identical empty fields fails. The actual ESP32 bundle predicate and full hardware handshake remain acceptance items. This is not a claim that dates absent from compact bundled trust anchors are checked.

## Local results

- X4 Pro target compiles with the unchanged SDK commit `bbd528ceb136696e009305377bba5bb1b079c1cf`.
- **170 native TLS assertions pass**, including repeated fixture/setup assertions; these are not 170 separate independent scenarios.
- The candidate's existing **43 message-protocol checks pass** after recompilation.
- Baseline controls reproduce the date-disabled library accepting expired and future certificates.
- The candidate accepts valid certificates and exact boundaries.
- Expired/future leaf and intermediate certificates are rejected.
- Real chain/hostname checks reject an untrusted issuer, wrong hostname and damaged signature.
- Clock failure with a plausible old floor, invalid time, stale freshness, uptime rollover, callback flag preservation and setup failures pass.
- UTC validation is unchanged by the display timezone.

The host harness runs the production adapter/date code against real Mbed TLS 3.6.6 X.509 parsing and chain verification with MBEDTLS_HAVE_TIME_DATE deliberately disabled. Synthetic signing keys exist only in the fixture generator's memory. Host stubs supply SNTP status/time and bundle attachment; this is not an ESP32 socket handshake.

Clang 21 diagnoses an upstream date-disabled-library pointer warning as an error. A host-library-only `-Wno-error=uninitialized-const-pointer` exception allowed that build. Candidate code remains checked with `-Wall -Wextra -Werror`; the firmware SDK is unchanged. The warning does not suppress any candidate test failure.

The X4 Pro build retains existing wolfSSL macro and upstream library warnings. No commit or hardware validation is claimed.

## Reproduce local tests

The firmware build is `pio run -e x4pro`, as described in the README.

For the native test, obtain official Mbed TLS tag `mbedtls-3.6.6` with its framework submodule; observed source commit is `0bebf8b8c7f07abe3571ded48a11aa907a1ffb20`, framework `dff9da04438d712f7647fd995bc90fadd0c0e2ce`. Disable MBEDTLS_HAVE_TIME_DATE in that test-only source with its config utility. Build its static libraries with programs/tests off, then:

```sh
cmake -S test/message_tls -B build/message-tls-host \
  -DMBEDTLS_SOURCE=/absolute/path/to/mbedtls-3.6.6 \
  -DMBEDTLS_BUILD=/absolute/path/to/mbedtls-host-build
cmake --build build/message-tls-host --parallel 4
build/message-tls-host/MessageTlsTest test/message_tls/fixtures
```

The pre-generated fixtures are included. Regeneration uses test/message_tls/generate_certificates.py with cryptography and produces new synthetic certificate signatures; expected validity decisions stay the same.

## Remaining acceptance

On a real X4 Pro, verify reading/context preservation, manual and timer HTTPS polling, valid/expired/future chains, wrong host/untrusted issuer, unavailable NTP, panel preservation on failures, Wi-Fi shutdown and battery behavior.
