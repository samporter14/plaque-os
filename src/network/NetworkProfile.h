#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "NetSecret.h"

// The network profile bundle the Mac sends (docs: SPEC-UNIVERSITY-WIFI.md sections 3 and 4): validation of every
// field, the EAP-TLS certificate and key checks (mbedTLS), and the files that describe the installed profile without
// its secrets. Pure: no Arduino or ESP-IDF headers, covered by test/network_profile.
namespace netprofile {

constexpr size_t MAX_BUNDLE_BYTES = 16384;
constexpr size_t MAX_NAME_CHARS = 40;
constexpr size_t MAX_SSID_BYTES = 32;
// esp_eap_client_set_identity() takes 1 to 127 bytes (ESP_ERR_INVALID_ARG from 128), so a 128-byte identity could be
// installed but never used. The contract's 1-128 is narrowed to what the reader can join with.
constexpr size_t MAX_IDENTITY_BYTES = 127;
constexpr size_t MAX_SERVER_DOMAIN_CHARS = 253;
constexpr size_t MAX_LABEL_CHARS = 63;
constexpr size_t MAX_CA_CERTS = 3;
constexpr size_t MAX_TIME_SERVERS = 3;
constexpr unsigned MIN_RSA_BITS = 2048;
// esp_eap_client_set_certificate_and_key() takes keys of at most 4096 bytes: an RSA key above 4096 bits does not fit.
constexpr unsigned MAX_RSA_BITS = 4096;
constexpr size_t MAX_SUBJECT_CHARS = 200;

// The shared error codes (section 3), in the contract's order.
enum class Error : uint8_t {
  None,
  TooLarge,
  BadJson,
  BadKind,
  BadSchema,
  BadField,
  NameInvalid,
  SsidInvalid,
  AuthUnsupported,
  PskInvalid,
  EapIdentityInvalid,
  ServerDomainInvalid,
  CaInvalid,
  ClientCertInvalid,
  ClientKeyInvalid,
  KeyEncrypted,
  KeyMismatch,
  ClientCertExpired,
  ClientCertNotYetValid,
  TimeServerInvalid,
  CardKeyRefused,
  Checksum,
  Declined,
  StorageFull,
  StorageFailed,
  Busy,
};
const char* errorCode(Error error);

enum class Auth : uint8_t { Open, Psk, EapTls };
enum class Institution : uint8_t { UmichIts, MichiganMedicineHits, Other };
enum class Category : uint8_t { RegisteredDevice, SharedPassword, DeviceCredential, EnterpriseCertificate };
enum class Source : uint8_t { Usb, Card };
// How the client key is encoded (the PEM label it came under): PKCS#8, PKCS#1 (RSA) or SEC1 (EC).
enum class KeyFormat : uint8_t { Pkcs8, Pkcs1, Sec1 };

const char* authName(Auth auth);
const char* institutionName(Institution institution);
const char* categoryName(Category category);
const char* sourceName(Source source);

struct Digest {
  uint8_t bytes[32] = {};
};

// Everything about a profile that may be shown, logged or written to the card: no PSK, no key.
struct Summary {
  bool installed = false;
  std::string profileId;
  std::string name;
  Institution institution = Institution::Other;
  Category category = Category::RegisteredDevice;
  std::string ssid;
  bool hidden = false;
  Auth auth = Auth::Open;
  std::vector<std::string> timeServers;  // empty: the reader's defaults
  bool useDhcpTime = false;
  int64_t createdAt = 0;  // the Mac's clock, informational only
  // EAP-TLS only.
  std::string serverDomain;
  std::vector<Digest> caSha256;  // SHA-256 of each CA certificate (DER)
  std::string clientCertSubject;
  bool hasCertDates = false;
  int64_t clientCertNotBefore = 0;  // unix seconds
  int64_t clientCertNotAfter = 0;
  Source installedVia = Source::Usb;
};

// A validated bundle: the summary plus what a join needs. Secrets live in SecretBytes and are zeroed when it goes.
struct Profile {
  Summary info;
  std::string identity;        // EAP-TLS: the outer identity IT supplied
  SecretBytes psk;             // PSK only: the passphrase or the 64 hex digits, as given
  std::vector<uint8_t> caDer;  // the CA certificates, DER, back to back
  std::vector<uint8_t> clientCertDer;
  SecretBytes keyDer;  // the client key, DER, as it was under its PEM label
  KeyFormat keyFormat = KeyFormat::Pkcs8;
  // Set once name, SSID and auth have validated, even if a later field fails: enough to say which bundle it was.
  bool described = false;
};

// The random source mbedTLS needs to check that the key matches the certificate (mbedtls_pk_check_pair).
using RandomFn = int (*)(void* context, unsigned char* out, size_t size);

struct ValidateOptions {
  // Unix seconds from a trusted clock, or 0: then the client certificate's dates are shown, never judged.
  int64_t trustedNow = 0;
  RandomFn random = nullptr;
  void* randomContext = nullptr;
  Source source = Source::Usb;
  // The kinds of network this board supports (plaque::Capabilities wifiOpen, wifiPsk, wifiEapTls); a profile of another
  // kind is refused with auth_unsupported. All on by default, as on both Plaque boards.
  bool allowOpen = true;
  bool allowPsk = true;
  bool allowEapTls = true;
};

// Parses and validates a bundle exactly as section 3 says. On success fills `out` and returns Error::None.
Error parseBundle(std::string_view text, const ValidateOptions& options, Profile& out);

// The card inbox carries open and PSK profiles only: a bundle with an `eap` object, or a private key anywhere in
// it, is refused (card_key_refused) before anything else is looked at.
Error parseCardBundle(std::string_view text, const ValidateOptions& options, Profile& out);

// /Lamina/network-profile.json for the installed profile, or {"schema": 1, "installed": false}. Ends in a newline.
std::string summaryJson(const Summary& summary);

// /Lamina/network-import.json. `at` is trusted unix time or 0 (written as null). `profile` is null when the bundle
// was not readable enough to say what it was.
std::string importResultJson(int64_t at, Error error, const Summary* profile);
// The same file while the person is being asked: {"result": "pending", "error": null, ...}. It replaces the result of
// an earlier import at once, so a file left on the card never describes an import it did not make; installing or
// declining then overwrites it.
std::string importPendingJson(int64_t at, const Summary* profile);

// Lowercase hex of a digest.
std::string digestHex(const Digest& digest);

// A copy of `text` that is safe to draw: every invisible or direction-changing character (Unicode Cc, Cf, Zl and Zp: a
// right-to-left override, a zero-width joiner, a line separator) is replaced by U+FFFD, and so is any byte that is not
// well-formed UTF-8. An SSID may legally carry such characters (validation refuses controls only, and the Mac agrees
// with that), so what is drawn is not what is stored; the stored value and its validation are untouched.
std::string displaySafe(std::string_view text);

// Hostname syntax: dot-separated labels of letters, digits and hyphens (1 to 63 characters, no hyphen at either end),
// at most 253 characters in all.
bool validHostname(std::string_view text);
bool validIpv4(std::string_view text);

// Splits back-to-back DER certificates. False when the bytes are not whole DER SEQUENCEs.
bool splitDer(const std::vector<uint8_t>& der, std::vector<std::pair<size_t, size_t>>& parts);

// Unix seconds of a UTC calendar time (proleptic Gregorian).
int64_t unixFromUtc(int year, int month, int day, int hour, int minute, int second);
// The UTC date of unix seconds as YYYY-MM-DD (the screens show certificate dates this way).
std::string isoDate(int64_t unixSeconds);

}  // namespace netprofile
