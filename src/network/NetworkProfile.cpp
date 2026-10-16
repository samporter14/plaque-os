#include "NetworkProfile.h"

#include <mbedtls/ecp.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/x509.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "NetCodec.h"
#include "NetJson.h"
#include "network/CertificateDates.h"

namespace netprofile {
namespace {

using netjson::Document;
using netjson::Node;
using netjson::Type;

constexpr char KIND[] = "plaque-network-profile";

constexpr const char* TOP_KEYS[] = {"kind",     "schema",       "profile_id",    "name",      "institution",
                                    "category", "ssid",         "hidden",        "auth",      "psk",
                                    "eap",      "time_servers", "use_dhcp_time", "created_at"};
constexpr const char* EAP_KEYS[] = {"identity", "server_domain", "ca_pem", "client_cert_pem", "client_key_pem"};

template <size_t N>
bool listed(const std::string& key, const char* const (&names)[N]) {
  for (const char* name : names)
    if (key == name) return true;
  return false;
}

// ---- text --------------------------------------------------------------------------------------------------------

// One UTF-8 character at text[i] (the JSON reader has checked the encoding already).
size_t nextCodePoint(const std::string_view text, const size_t i, uint32_t& cp) {
  const auto lead = static_cast<uint8_t>(text[i]);
  size_t length = 1;
  if (lead < 0x80) {
    cp = lead;
  } else if (lead < 0xE0) {
    length = 2;
    cp = lead & 0x1F;
  } else if (lead < 0xF0) {
    length = 3;
    cp = lead & 0x0F;
  } else {
    length = 4;
    cp = lead & 0x07;
  }
  if (i + length > text.size()) return 0;
  for (size_t k = 1; k < length; ++k) cp = (cp << 6) | (static_cast<uint8_t>(text[i + k]) & 0x3F);
  return length;
}

bool control(const uint32_t cp) { return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F); }  // Unicode Cc

// Unicode Cc, Cf, Zl and Zp: control and invisible format characters, and the line and paragraph separators.
bool unprintable(const uint32_t cp) {
  if (control(cp)) return true;
  switch (cp) {
    case 0x00AD:
    case 0x061C:
    case 0x06DD:
    case 0x070F:
    case 0x08E2:
    case 0x180E:
    case 0x2028:
    case 0x2029:
    case 0xFEFF:
    case 0x110BD:
    case 0x110CD:
    case 0xE0001:
      return true;
    default:
      break;
  }
  return (cp >= 0x0600 && cp <= 0x0605) || (cp >= 0x0890 && cp <= 0x0891) || (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) ||
         (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0x13430 && cp <= 0x1343F) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
         (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0020 && cp <= 0xE007F);
}

bool validName(const std::string_view text) {
  size_t characters = 0;
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = 0;
    const size_t length = nextCodePoint(text, i, cp);
    if (length == 0 || unprintable(cp)) return false;
    i += length;
    ++characters;
  }
  return characters >= 1 && characters <= MAX_NAME_CHARS;
}

bool validSsid(const std::string_view text) {
  if (text.empty() || text.size() > MAX_SSID_BYTES) return false;
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = 0;
    const size_t length = nextCodePoint(text, i, cp);
    if (length == 0 || control(cp)) return false;
    i += length;
  }
  return true;
}

bool printableAscii(const std::string_view text) {
  for (const char c : text)
    if (c < 0x20 || c > 0x7E) return false;
  return true;
}

bool hexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool validProfileId(const std::string_view text) {
  if (text.size() != 8) return false;
  for (const char c : text)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

bool validPsk(const std::string_view text) {
  if (text.size() == 64) return std::all_of(text.begin(), text.end(), hexDigit);
  return text.size() >= 8 && text.size() <= 63 && printableAscii(text);
}

// ---- PEM -----------------------------------------------------------------------------------------------------------

struct PemBlock {
  std::string_view label;
  std::string_view body;   // between the BEGIN and END lines, headers included
  bool encrypted = false;  // a "Proc-Type: 4,ENCRYPTED" header (the legacy OpenSSL encryption)
  bool headers = false;    // RFC 1421 headers of any kind
};

// The PEM blocks in a text. Text outside the blocks (an "issuer=" line from openssl, blank lines) is ignored. False
// when a block has no matching END line.
bool scanPem(const std::string_view text, std::vector<PemBlock>& blocks) {
  constexpr std::string_view BEGIN = "-----BEGIN ";
  constexpr std::string_view DASHES = "-----";
  size_t pos = 0;
  while (true) {
    const size_t begin = text.find(BEGIN, pos);
    if (begin == std::string_view::npos) return true;
    const size_t labelStart = begin + BEGIN.size();
    const size_t labelEnd = text.find(DASHES, labelStart);
    if (labelEnd == std::string_view::npos) return false;
    PemBlock block;
    block.label = text.substr(labelStart, labelEnd - labelStart);
    if (block.label.find('\n') != std::string_view::npos) return false;
    const size_t bodyStart = labelEnd + DASHES.size();
    std::string end = "-----END ";
    end.append(block.label.data(), block.label.size());
    end += "-----";
    const size_t bodyEnd = text.find(end, bodyStart);
    if (bodyEnd == std::string_view::npos) return false;
    block.body = text.substr(bodyStart, bodyEnd - bodyStart);
    block.headers = block.body.find(':') != std::string_view::npos;
    block.encrypted = block.headers && block.body.find("ENCRYPTED") != std::string_view::npos;
    blocks.push_back(block);
    pos = bodyEnd + end.size();
  }
}

bool decodeBody(const PemBlock& block, std::vector<uint8_t>& out) {
  out.assign(block.body.size() * 3 / 4 + 3, 0);
  size_t written = 0;
  if (!netcodec::base64Decode(block.body, out.data(), out.size(), written, true) || written == 0) {
    out.clear();
    return false;
  }
  out.resize(written);
  return true;
}

bool decodeSecretBody(const PemBlock& block, SecretBytes& out) {
  if (!out.allocate(block.body.size() * 3 / 4 + 3)) return false;
  size_t written = 0;
  if (!netcodec::base64Decode(block.body, out.data(), out.size(), written, true) || written == 0) {
    out.reset();
    return false;
  }
  out.shrink(written);
  return true;
}

// ---- certificates and keys -----------------------------------------------------------------------------------------

class Certificate {
 public:
  mbedtls_x509_crt crt;
  Certificate() { mbedtls_x509_crt_init(&crt); }
  ~Certificate() { mbedtls_x509_crt_free(&crt); }
  Certificate(const Certificate&) = delete;
  Certificate& operator=(const Certificate&) = delete;
  bool parse(const std::vector<uint8_t>& der) { return mbedtls_x509_crt_parse_der(&crt, der.data(), der.size()) == 0; }
};

class Key {
 public:
  mbedtls_pk_context pk;
  Key() { mbedtls_pk_init(&pk); }
  ~Key() { mbedtls_pk_free(&pk); }  // zeroes the key material
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
};

int64_t unixOf(const mbedtls_x509_time& t) { return unixFromUtc(t.year, t.mon, t.day, t.hour, t.min, t.sec); }

message_tls::UtcDate dateOf(const mbedtls_x509_time& t) { return {t.year, t.mon, t.day, t.hour, t.min, t.sec}; }

Error checkCa(const std::string_view pem, Profile& out) {
  std::vector<PemBlock> blocks;
  if (!scanPem(pem, blocks) || blocks.empty() || blocks.size() > MAX_CA_CERTS) return Error::CaInvalid;
  out.caDer.clear();
  out.info.caSha256.clear();
  for (const PemBlock& block : blocks) {
    if (block.label != "CERTIFICATE" || block.headers) return Error::CaInvalid;
    std::vector<uint8_t> der;
    if (!decodeBody(block, der)) return Error::CaInvalid;
    Certificate certificate;
    if (!certificate.parse(der)) return Error::CaInvalid;
    // A CA certificate says so: basicConstraints with cA TRUE.
    if (!mbedtls_x509_crt_has_ext_type(&certificate.crt, MBEDTLS_X509_EXT_BASIC_CONSTRAINTS) ||
        mbedtls_x509_crt_get_ca_istrue(&certificate.crt) != 1)
      return Error::CaInvalid;
    Digest digest;
    if (!netcodec::sha256(der.data(), der.size(), digest.bytes)) return Error::CaInvalid;
    out.info.caSha256.push_back(digest);
    out.caDer.insert(out.caDer.end(), der.begin(), der.end());
  }
  return Error::None;
}

std::string printableSubject(const mbedtls_x509_name& subject) {
  char buffer[512];
  buffer[0] = '\0';
  mbedtls_x509_dn_gets(buffer, sizeof(buffer), &subject);  // a cut-off name is still a name to show
  buffer[sizeof(buffer) - 1] = '\0';
  std::string text;
  for (const char* c = buffer; *c && text.size() < MAX_SUBJECT_CHARS; ++c)
    text.push_back(*c >= 0x20 && *c < 0x7F ? *c : '?');
  return text;
}

Error checkClientCert(const std::string_view pem, Profile& out, Certificate& certificate) {
  std::vector<PemBlock> blocks;
  if (!scanPem(pem, blocks) || blocks.size() != 1 || blocks[0].label != "CERTIFICATE" || blocks[0].headers)
    return Error::ClientCertInvalid;
  if (!decodeBody(blocks[0], out.clientCertDer) || !certificate.parse(out.clientCertDer))
    return Error::ClientCertInvalid;
  out.info.clientCertSubject = printableSubject(certificate.crt.subject);
  out.info.hasCertDates = true;
  out.info.clientCertNotBefore = unixOf(certificate.crt.valid_from);
  out.info.clientCertNotAfter = unixOf(certificate.crt.valid_to);
  return Error::None;
}

Error checkKey(const std::string_view pem, const ValidateOptions& options, Profile& out, const Certificate& cert) {
  std::vector<PemBlock> blocks;
  if (!scanPem(pem, blocks)) return Error::ClientKeyInvalid;
  const PemBlock* key = nullptr;
  for (const PemBlock& block : blocks) {
    if (block.label == "ENCRYPTED PRIVATE KEY") return Error::KeyEncrypted;
    if (block.label == "EC PARAMETERS") continue;  // openssl writes the curve name ahead of an EC key
    const bool keyLabel =
        block.label == "PRIVATE KEY" || block.label == "RSA PRIVATE KEY" || block.label == "EC PRIVATE KEY";
    if (!keyLabel || key) return Error::ClientKeyInvalid;  // something else, or a second key
    key = &block;
  }
  if (!key) return Error::ClientKeyInvalid;
  if (key->encrypted) return Error::KeyEncrypted;
  if (key->headers) return Error::ClientKeyInvalid;
  out.keyFormat = key->label == "PRIVATE KEY"       ? KeyFormat::Pkcs8
                  : key->label == "RSA PRIVATE KEY" ? KeyFormat::Pkcs1
                                                    : KeyFormat::Sec1;
  if (!decodeSecretBody(*key, out.keyDer)) return Error::ClientKeyInvalid;

  Key parsed;
  const int result = mbedtls_pk_parse_key(&parsed.pk, out.keyDer.data(), out.keyDer.size(), nullptr, 0, options.random,
                                          options.randomContext);
  if (result == MBEDTLS_ERR_PK_PASSWORD_REQUIRED || result == MBEDTLS_ERR_PK_PASSWORD_MISMATCH)
    return Error::KeyEncrypted;
  if (result != 0) return Error::ClientKeyInvalid;
  const mbedtls_pk_type_t type = mbedtls_pk_get_type(&parsed.pk);
  if (type == MBEDTLS_PK_RSA) {
    if (out.keyFormat == KeyFormat::Sec1) return Error::ClientKeyInvalid;
    const size_t bits = mbedtls_pk_get_bitlen(&parsed.pk);
    if (bits < MIN_RSA_BITS || bits > MAX_RSA_BITS) return Error::ClientKeyInvalid;
  } else if (type == MBEDTLS_PK_ECKEY) {
    if (out.keyFormat == KeyFormat::Pkcs1) return Error::ClientKeyInvalid;
    const mbedtls_ecp_group_id curve = mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(parsed.pk));
    if (curve != MBEDTLS_ECP_DP_SECP256R1 && curve != MBEDTLS_ECP_DP_SECP384R1) return Error::ClientKeyInvalid;
  } else {
    return Error::ClientKeyInvalid;
  }
  if (!options.random || mbedtls_pk_check_pair(&cert.crt.pk, &parsed.pk, options.random, options.randomContext) != 0)
    return Error::KeyMismatch;
  return Error::None;
}

// The `eap` object. Cheap text checks first, then the certificates and the key.
Error checkEap(const Document& doc, const Node& eap, const ValidateOptions& options, Profile& out) {
  if (eap.type != Type::Object) return Error::BadField;
  for (const Node* member = doc.first(eap); member; member = doc.next(*member))
    if (!listed(member->key, EAP_KEYS)) return Error::BadField;

  const Node* identity = doc.member(eap, "identity");
  if (!identity || identity->type != Type::String) return Error::EapIdentityInvalid;
  out.identity = doc.stringValue(*identity);
  if (out.identity.empty() || out.identity.size() > MAX_IDENTITY_BYTES || !printableAscii(out.identity))
    return Error::EapIdentityInvalid;

  const Node* domain = doc.member(eap, "server_domain");
  if (!domain || domain->type != Type::String) return Error::ServerDomainInvalid;
  out.info.serverDomain = doc.stringValue(*domain);
  if (!validHostname(out.info.serverDomain)) return Error::ServerDomainInvalid;

  const Node* ca = doc.member(eap, "ca_pem");
  if (!ca || ca->type != Type::String) return Error::CaInvalid;
  if (const Error error = checkCa(doc.stringValue(*ca), out); error != Error::None) return error;

  const Node* certNode = doc.member(eap, "client_cert_pem");
  if (!certNode || certNode->type != Type::String) return Error::ClientCertInvalid;
  Certificate certificate;
  if (const Error error = checkClientCert(doc.stringValue(*certNode), out, certificate); error != Error::None)
    return error;

  const Node* keyNode = doc.member(eap, "client_key_pem");
  if (!keyNode || keyNode->type != Type::String) return Error::ClientKeyInvalid;
  SecretBytes keyPem;
  if (!doc.secretValue(*keyNode, keyPem)) return Error::ClientKeyInvalid;
  if (const Error error = checkKey(keyPem.view(), options, out, certificate); error != Error::None) return error;

  // Without a trusted clock the dates are shown, not judged.
  if (options.trustedNow > 0) {
    switch (message_tls::checkDates(dateOf(certificate.crt.valid_from), dateOf(certificate.crt.valid_to),
                                    options.trustedNow)) {
      case message_tls::DateResult::Valid:
      case message_tls::DateResult::InvalidClock:
        break;
      case message_tls::DateResult::Expired:
        return Error::ClientCertExpired;
      case message_tls::DateResult::Future:
        return Error::ClientCertNotYetValid;
      case message_tls::DateResult::Malformed:
        return Error::ClientCertInvalid;
    }
  }
  return Error::None;
}

Error checkTimeServers(const Document& doc, const Node* node, Profile& out) {
  out.info.timeServers.clear();
  if (!node || node->type == Type::Null) return Error::None;
  if (node->type != Type::Array || node->childCount > MAX_TIME_SERVERS) return Error::TimeServerInvalid;
  for (const Node* item = doc.first(*node); item; item = doc.next(*item)) {
    if (item->type != Type::String) return Error::TimeServerInvalid;
    std::string server = doc.stringValue(*item);
    if (!validHostname(server) && !validIpv4(server)) return Error::TimeServerInvalid;
    out.info.timeServers.push_back(std::move(server));
  }
  return Error::None;
}

// An optional bool: absent and null read as false.
bool optionalBool(const Node* node, bool& out) {
  out = false;
  if (!node || node->type == Type::Null) return true;
  if (node->type != Type::Bool) return false;
  out = node->boolean;
  return true;
}

bool absentOrNull(const Node* node) { return !node || node->type == Type::Null; }

// Any string in the document (at any depth) that names a private key once its escapes are decoded.
bool hidesKey(const Document& doc, const Node& node) {
  if (node.type == Type::String) {
    SecretBytes value;
    return doc.secretValue(node, value) && value.view().find("PRIVATE KEY") != std::string_view::npos;
  }
  for (const Node* item = doc.first(node); item; item = doc.next(*item))
    if (hidesKey(doc, *item)) return true;
  return false;
}

void appendNullableString(std::string& out, const std::string& text) {
  if (text.empty()) {
    out += "null";
  } else {
    netjson::appendString(out, text);
  }
}

int64_t daysFromCivil(int year, const int month, const int day) {
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yearOfEra = year - era * 400;
  const int64_t dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

}  // namespace

// ---- names
// -----------------------------------------------------------------------------------------------------------

const char* errorCode(const Error error) {
  switch (error) {
    case Error::None:
      return "none";
    case Error::TooLarge:
      return "too_large";
    case Error::BadJson:
      return "bad_json";
    case Error::BadKind:
      return "bad_kind";
    case Error::BadSchema:
      return "bad_schema";
    case Error::BadField:
      return "bad_field";
    case Error::NameInvalid:
      return "name_invalid";
    case Error::SsidInvalid:
      return "ssid_invalid";
    case Error::AuthUnsupported:
      return "auth_unsupported";
    case Error::PskInvalid:
      return "psk_invalid";
    case Error::EapIdentityInvalid:
      return "eap_identity_invalid";
    case Error::ServerDomainInvalid:
      return "server_domain_invalid";
    case Error::CaInvalid:
      return "ca_invalid";
    case Error::ClientCertInvalid:
      return "client_cert_invalid";
    case Error::ClientKeyInvalid:
      return "client_key_invalid";
    case Error::KeyEncrypted:
      return "key_encrypted";
    case Error::KeyMismatch:
      return "key_mismatch";
    case Error::ClientCertExpired:
      return "client_cert_expired";
    case Error::ClientCertNotYetValid:
      return "client_cert_not_yet_valid";
    case Error::TimeServerInvalid:
      return "time_server_invalid";
    case Error::CardKeyRefused:
      return "card_key_refused";
    case Error::Checksum:
      return "checksum";
    case Error::Declined:
      return "declined";
    case Error::StorageFull:
      return "storage_full";
    case Error::StorageFailed:
      return "storage_failed";
    case Error::Busy:
      return "busy";
  }
  return "storage_failed";
}

const char* authName(const Auth auth) {
  switch (auth) {
    case Auth::Open:
      return "open";
    case Auth::Psk:
      return "psk";
    case Auth::EapTls:
      return "eap-tls";
  }
  return "open";
}

const char* institutionName(const Institution institution) {
  switch (institution) {
    case Institution::UmichIts:
      return "umich-its";
    case Institution::MichiganMedicineHits:
      return "michigan-medicine-hits";
    case Institution::Other:
      break;
  }
  return "other";
}

const char* categoryName(const Category category) {
  switch (category) {
    case Category::RegisteredDevice:
      return "registered-device";
    case Category::SharedPassword:
      return "shared-password";
    case Category::DeviceCredential:
      return "device-credential";
    case Category::EnterpriseCertificate:
      return "enterprise-certificate";
  }
  return "registered-device";
}

const char* sourceName(const Source source) { return source == Source::Card ? "card" : "usb"; }

// ---- syntax --------------------------------------------------------------------------------------------------------

bool validHostname(const std::string_view text) {
  if (text.empty() || text.size() > MAX_SERVER_DOMAIN_CHARS) return false;
  size_t labelStart = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i < text.size() && text[i] != '.') {
      const char c = text[i];
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
      continue;
    }
    const size_t length = i - labelStart;
    if (length == 0 || length > MAX_LABEL_CHARS) return false;
    if (text[labelStart] == '-' || text[i - 1] == '-') return false;
    labelStart = i + 1;
  }
  return true;
}

bool validIpv4(const std::string_view text) {
  int parts = 0;
  size_t i = 0;
  while (true) {
    const size_t start = i;
    int value = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
      value = value * 10 + (text[i] - '0');
      if (value > 255 || i - start >= 3) return false;
      ++i;
    }
    const size_t digits = i - start;
    if (digits == 0 || (digits > 1 && text[start] == '0')) return false;
    ++parts;
    if (i == text.size()) return parts == 4;
    if (text[i] != '.' || parts == 4) return false;
    ++i;
  }
}

bool splitDer(const std::vector<uint8_t>& der, std::vector<std::pair<size_t, size_t>>& parts) {
  parts.clear();
  size_t pos = 0;
  while (pos < der.size()) {
    if (der[pos] != 0x30 || pos + 2 > der.size()) return false;
    size_t length = der[pos + 1];
    size_t header = 2;
    if (length & 0x80) {
      const size_t count = length & 0x7F;
      if (count == 0 || count > 3 || pos + 2 + count > der.size()) return false;
      length = 0;
      for (size_t k = 0; k < count; ++k) length = (length << 8) | der[pos + 2 + k];
      header += count;
    }
    if (pos + header + length > der.size()) return false;
    parts.emplace_back(pos, header + length);
    pos += header + length;
  }
  return !parts.empty();
}

int64_t unixFromUtc(const int year, const int month, const int day, const int hour, const int minute,
                    const int second) {
  return daysFromCivil(year, month, day) * 86400 + int64_t{hour} * 3600 + int64_t{minute} * 60 + second;
}

std::string isoDate(const int64_t unixSeconds) {
  // Howard Hinnant's civil_from_days.
  int64_t days = unixSeconds / 86400;
  if (unixSeconds % 86400 < 0) --days;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t dayOfEra = days - era * 146097;
  const int64_t yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const int64_t mp = (5 * dayOfYear + 2) / 153;
  const int64_t day = dayOfYear - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);
  char text[24];
  snprintf(text, sizeof(text), "%04lld-%02lld-%02lld", static_cast<long long>(year), static_cast<long long>(month),
           static_cast<long long>(day));
  return text;
}

std::string digestHex(const Digest& digest) {
  char hex[65];
  netcodec::toHex(digest.bytes, sizeof(digest.bytes), hex);
  return hex;
}

// ---- the bundle ----------------------------------------------------------------------------------------------------

Error parseBundle(const std::string_view text, const ValidateOptions& options, Profile& out) {
  out = Profile();
  out.info.installedVia = options.source;
  if (text.size() > MAX_BUNDLE_BYTES) return Error::TooLarge;
  Document doc;
  if (!doc.parse(text)) return Error::BadJson;
  const Node& root = doc.root();
  if (root.type != Type::Object) return Error::BadJson;

  // kind and schema first, so a bundle from a future Mac reads as what it is rather than as unknown fields.
  const Node* kind = doc.member(root, "kind");
  if (!kind || kind->type != Type::String || doc.stringValue(*kind) != KIND) return Error::BadKind;
  const Node* schema = doc.member(root, "schema");
  int64_t schemaValue = 0;
  if (!schema || !doc.integerValue(*schema, schemaValue) || schemaValue != 1) return Error::BadSchema;
  for (const Node* member = doc.first(root); member; member = doc.next(*member))
    if (!listed(member->key, TOP_KEYS)) return Error::BadField;

  const Node* id = doc.member(root, "profile_id");
  if (!id || id->type != Type::String) return Error::BadField;
  out.info.profileId = doc.stringValue(*id);
  if (!validProfileId(out.info.profileId)) return Error::BadField;

  const Node* name = doc.member(root, "name");
  if (!name || name->type != Type::String) return Error::NameInvalid;
  out.info.name = doc.stringValue(*name);
  if (!validName(out.info.name)) return Error::NameInvalid;

  const Node* institution = doc.member(root, "institution");
  if (!institution || institution->type != Type::String) return Error::BadField;
  const std::string institutionText = doc.stringValue(*institution);
  if (institutionText == "umich-its") {
    out.info.institution = Institution::UmichIts;
  } else if (institutionText == "michigan-medicine-hits") {
    out.info.institution = Institution::MichiganMedicineHits;
  } else if (institutionText == "other") {
    out.info.institution = Institution::Other;
  } else {
    return Error::BadField;
  }

  const Node* category = doc.member(root, "category");
  if (!category || category->type != Type::String) return Error::BadField;
  const std::string categoryText = doc.stringValue(*category);
  if (categoryText == "registered-device") {
    out.info.category = Category::RegisteredDevice;
  } else if (categoryText == "shared-password") {
    out.info.category = Category::SharedPassword;
  } else if (categoryText == "device-credential") {
    out.info.category = Category::DeviceCredential;
  } else if (categoryText == "enterprise-certificate") {
    out.info.category = Category::EnterpriseCertificate;
  } else {
    return Error::BadField;
  }

  const Node* ssid = doc.member(root, "ssid");
  if (!ssid || ssid->type != Type::String) return Error::SsidInvalid;
  out.info.ssid = doc.stringValue(*ssid);
  if (!validSsid(out.info.ssid)) return Error::SsidInvalid;

  if (!optionalBool(doc.member(root, "hidden"), out.info.hidden)) return Error::BadField;

  const Node* auth = doc.member(root, "auth");
  if (!auth || auth->type != Type::String) return Error::AuthUnsupported;
  const std::string authText = doc.stringValue(*auth);
  if (authText == "open") {
    out.info.auth = Auth::Open;
  } else if (authText == "psk") {
    out.info.auth = Auth::Psk;
  } else if (authText == "eap-tls") {
    out.info.auth = Auth::EapTls;
  } else {
    return Error::AuthUnsupported;
  }
  out.described = true;
  // A kind of network this board does not support is refused as one the contract does not know (auth_unsupported),
  // before any of its secrets are read.
  if ((out.info.auth == Auth::Open && !options.allowOpen) || (out.info.auth == Auth::Psk && !options.allowPsk) ||
      (out.info.auth == Auth::EapTls && !options.allowEapTls))
    return Error::AuthUnsupported;

  const Node* psk = doc.member(root, "psk");
  if (out.info.auth == Auth::Psk) {
    if (!psk || psk->type != Type::String || !doc.secretValue(*psk, out.psk) || !validPsk(out.psk.view())) {
      out.psk.reset();
      return Error::PskInvalid;
    }
  } else if (!absentOrNull(psk)) {
    return Error::BadField;
  }

  const Node* eap = doc.member(root, "eap");
  if (out.info.auth == Auth::EapTls) {
    if (absentOrNull(eap)) return Error::BadField;
  } else if (!absentOrNull(eap)) {
    return Error::BadField;
  }

  if (const Error error = checkTimeServers(doc, doc.member(root, "time_servers"), out); error != Error::None)
    return error;
  if (!optionalBool(doc.member(root, "use_dhcp_time"), out.info.useDhcpTime)) return Error::BadField;

  const Node* created = doc.member(root, "created_at");
  if (!created || !doc.integerValue(*created, out.info.createdAt) || out.info.createdAt < 0) return Error::BadField;

  if (out.info.auth == Auth::EapTls) {
    if (const Error error = checkEap(doc, *eap, options, out); error != Error::None) return error;
  }
  out.info.installed = true;
  return Error::None;
}

Error parseCardBundle(const std::string_view text, const ValidateOptions& options, Profile& out) {
  out = Profile();
  // A private key never travels on the card: refused on sight, whatever else the file is.
  if (text.find("PRIVATE KEY") != std::string_view::npos) return Error::CardKeyRefused;
  if (text.size() > MAX_BUNDLE_BYTES) return Error::TooLarge;
  Document doc;
  if (!doc.parse(text)) return Error::BadJson;
  const Node& root = doc.root();
  if (root.type == Type::Object && !absentOrNull(doc.member(root, "eap"))) return Error::CardKeyRefused;
  // A key hidden behind JSON escapes in any string is still a key.
  if (hidesKey(doc, root)) return Error::CardKeyRefused;
  return parseBundle(text, options, out);
}

// ---- files ---------------------------------------------------------------------------------------------------------

std::string displaySafe(const std::string_view text) {
  constexpr char REPLACEMENT[] = "\xEF\xBF\xBD";  // U+FFFD
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const auto lead = static_cast<uint8_t>(text[i]);
    const size_t length = lead < 0x80                   ? 1
                          : lead >= 0xC2 && lead < 0xE0 ? 2
                          : lead >= 0xE0 && lead < 0xF0 ? 3
                          : lead >= 0xF0 && lead < 0xF5 ? 4
                                                        : 0;
    bool wellFormed = length != 0 && i + length <= text.size();
    for (size_t k = 1; wellFormed && k < length; ++k) wellFormed = (static_cast<uint8_t>(text[i + k]) & 0xC0) == 0x80;
    uint32_t cp = 0;
    if (wellFormed) {
      nextCodePoint(text, i, cp);
      // Overlong forms, surrogates and values past U+10FFFF are not text either.
      wellFormed = !(length == 3 && cp < 0x800) && !(length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) &&
                   !(cp >= 0xD800 && cp <= 0xDFFF);
    }
    if (!wellFormed) {
      out += REPLACEMENT;
      ++i;
      continue;
    }
    if (unprintable(cp)) {
      out += REPLACEMENT;
    } else {
      out.append(text.substr(i, length));
    }
    i += length;
  }
  return out;
}

std::string summaryJson(const Summary& s) {
  if (!s.installed) return "{\"schema\":1,\"installed\":false}\n";
  std::string out;
  out.reserve(512);
  out += "{\"schema\":1,\"installed\":true,\"profile_id\":";
  netjson::appendString(out, s.profileId);
  out += ",\"name\":";
  netjson::appendString(out, s.name);
  out += ",\"institution\":";
  netjson::appendString(out, institutionName(s.institution));
  out += ",\"category\":";
  netjson::appendString(out, categoryName(s.category));
  out += ",\"ssid\":";
  netjson::appendString(out, s.ssid);
  out += ",\"auth\":";
  netjson::appendString(out, authName(s.auth));
  out += ",\"time_servers\":[";
  for (size_t i = 0; i < s.timeServers.size(); ++i) {
    if (i) out += ',';
    netjson::appendString(out, s.timeServers[i]);
  }
  out += "],\"use_dhcp_time\":";
  out += s.useDhcpTime ? "true" : "false";
  out += ",\"server_domain\":";
  appendNullableString(out, s.serverDomain);
  out += ",\"ca_sha256\":[";
  for (size_t i = 0; i < s.caSha256.size(); ++i) {
    if (i) out += ',';
    netjson::appendString(out, digestHex(s.caSha256[i]));
  }
  out += "],\"client_cert_subject\":";
  appendNullableString(out, s.clientCertSubject);
  out += ",\"client_cert_not_after\":";
  out += s.hasCertDates ? std::to_string(s.clientCertNotAfter) : "null";
  out += ",\"installed_via\":";
  netjson::appendString(out, sourceName(s.installedVia));
  out += "}\n";
  return out;
}
namespace {
// network-import.json with its result word and error code given.
std::string importJson(const int64_t at, const char* result, const char* errorText, const Summary* profile) {
  std::string out = "{\"schema\":1,\"at\":";
  out += at > 0 ? std::to_string(at) : "null";
  out += ",\"result\":\"";
  out += result;
  out += "\",\"error\":";
  if (errorText) {
    netjson::appendString(out, errorText);
  } else {
    out += "null";
  }
  out += ",\"profile\":";
  if (profile) {
    out += "{\"name\":";
    netjson::appendString(out, profile->name);
    out += ",\"ssid\":";
    netjson::appendString(out, profile->ssid);
    out += ",\"auth\":";
    netjson::appendString(out, authName(profile->auth));
    out += '}';
  } else {
    out += "null";
  }
  out += "}\n";
  return out;
}
}  // namespace

std::string importResultJson(const int64_t at, const Error error, const Summary* profile) {
  if (error == Error::None) return importJson(at, "installed", nullptr, profile);
  return importJson(at, "rejected", errorCode(error), profile);
}

std::string importPendingJson(const int64_t at, const Summary* profile) {
  return importJson(at, "pending", nullptr, profile);
}

}  // namespace netprofile
