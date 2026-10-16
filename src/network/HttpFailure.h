#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Why a bounded mailbox request did not succeed, told apart from what esp_http_client and ESP-TLS report. Pure: the
// error codes are mirrored here (HttpDownloader.cpp checks them against the SDK headers at compile time) so the host
// tests cover the mapping (test/http_failure).
namespace httpfail {

enum class Failure : uint8_t {
  None,         // the request succeeded
  Preflight,    // never sent: not an http(s) URL, no clock permission for HTTPS, too little heap, no client
  Connect,      // the name lookup or the TCP connection failed, or the connection dropped before the headers
  Tls,          // the TLS handshake failed for a reason other than the certificate
  Certificate,  // the certificate failed verification: chain, host name or dates
  Timeout,      // the connection, the handshake or the headers did not arrive within the socket timeout
  Status,       // the server answered with a status the caller does not take (see Outcome::status)
  Redirect,     // the server answered 3xx; it is never followed (see Outcome::locationHost)
  BadLength,    // 200 without a Content-Length the caller accepts
};
const char* failureName(Failure failure);

// What the last request found. locationHost is set from a Location header in the reply, lowercased, without any
// user name, password, port, path or query; empty when there was none or it named no host (a relative reference).
struct Outcome {
  Failure failure = Failure::None;
  int status = 0;  // the HTTP status, 0 when no reply was read
  char locationHost[254] = {};
};

// ---- the SDK's codes, mirrored --------------------------------------------------------------------------------------

constexpr int HTTP_ERR_EAGAIN = 0x7007;                  // ESP_ERR_HTTP_EAGAIN
constexpr int TLS_ERR_CANNOT_RESOLVE_HOSTNAME = 0x8001;  // ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME
constexpr int TLS_ERR_CANNOT_CREATE_SOCKET = 0x8002;
constexpr int TLS_ERR_UNSUPPORTED_PROTOCOL_FAMILY = 0x8003;
constexpr int TLS_ERR_FAILED_CONNECT_TO_HOST = 0x8004;
constexpr int TLS_ERR_SOCKET_SETOPT_FAILED = 0x8005;
constexpr int TLS_ERR_CONNECTION_TIMEOUT = 0x8006;
constexpr int TLS_ERR_TCP_CLOSED_FIN = 0x8008;
constexpr int TLS_ERR_SERVER_HANDSHAKE_TIMEOUT = 0x8009;
constexpr int TLS_ERR_MBEDTLS_FIRST = 0x8010;  // ESP_ERR_MBEDTLS_CERT_PARTLY_OK
constexpr int TLS_ERR_MBEDTLS_LAST = 0x801D;   // ESP_ERR_MBEDTLS_SSL_READ_FAILED
constexpr int TLS_ERR_MBEDTLS_HANDSHAKE_FAILED = 0x801A;
constexpr int MBEDTLS_CERT_VERIFY_FAILED = 0x2700;  // MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, magnitude
constexpr int MBEDTLS_SSL_TIMEOUT = 0x6800;         // MBEDTLS_ERR_SSL_TIMEOUT, magnitude
constexpr int ERRNO_EAGAIN = 11;
constexpr int ERRNO_ETIMEDOUT = 116;

// What a failed esp_http_client_open() left behind.
struct TransportError {
  int tlsError = 0;     // esp_http_client_get_and_clear_last_tls_error(): ESP_ERR_ESP_TLS_* or ESP_ERR_MBEDTLS_*
  int tlsCode = 0;      // the mbedTLS error behind it (ESP-TLS stores its magnitude)
  int certFlags = 0;    // the mbedTLS verification flags, 0 when verification did not run or passed
  int socketErrno = 0;  // esp_http_client_get_errno()
};
Failure classifyTransport(const TransportError& error);

// esp_http_client_fetch_headers() returned a negative value after a successful open.
Failure classifyHeaderRead(int64_t result);

enum class StatusClass : uint8_t { Ok, Unauthorized, NotFound, Gone, ServerError, Redirect, Other };
StatusClass classifyStatus(int status);

// The host a Location header names (see Outcome::locationHost). False, with out empty, for a relative reference or
// anything that is not a plain host name or IP literal.
bool locationHost(std::string_view location, char* out, size_t outSize);

}  // namespace httpfail
