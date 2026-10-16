#include "HttpFailure.h"

#include <cstdlib>

namespace httpfail {

const char* failureName(const Failure failure) {
  switch (failure) {
    case Failure::None:
      return "none";
    case Failure::Preflight:
      return "preflight";
    case Failure::Connect:
      return "connect";
    case Failure::Tls:
      return "tls";
    case Failure::Certificate:
      return "certificate";
    case Failure::Timeout:
      return "timeout";
    case Failure::Status:
      return "status";
    case Failure::Redirect:
      return "redirect";
    case Failure::BadLength:
      return "length";
  }
  return "";
}

Failure classifyTransport(const TransportError& error) {
  const bool timedOut = error.socketErrno == ERRNO_EAGAIN || error.socketErrno == ERRNO_ETIMEDOUT;
  switch (error.tlsError) {
    case TLS_ERR_CONNECTION_TIMEOUT:
    case TLS_ERR_SERVER_HANDSHAKE_TIMEOUT:
      return Failure::Timeout;
    case TLS_ERR_CANNOT_RESOLVE_HOSTNAME:
    case TLS_ERR_CANNOT_CREATE_SOCKET:
    case TLS_ERR_UNSUPPORTED_PROTOCOL_FAMILY:
    case TLS_ERR_FAILED_CONNECT_TO_HOST:
    case TLS_ERR_SOCKET_SETOPT_FAILED:
    case TLS_ERR_TCP_CLOSED_FIN:
      return timedOut ? Failure::Timeout : Failure::Connect;
    default:
      break;
  }
  // Verification flags mean the handshake reached the certificate and refused it, whatever error code followed.
  const int code = std::abs(error.tlsCode);
  if (error.certFlags != 0 || code == MBEDTLS_CERT_VERIFY_FAILED) return Failure::Certificate;
  if (code == MBEDTLS_SSL_TIMEOUT) return Failure::Timeout;
  if (error.tlsError >= TLS_ERR_MBEDTLS_FIRST && error.tlsError <= TLS_ERR_MBEDTLS_LAST)
    return timedOut ? Failure::Timeout : Failure::Tls;
  return timedOut ? Failure::Timeout : Failure::Connect;
}

Failure classifyHeaderRead(const int64_t result) {
  return result == -HTTP_ERR_EAGAIN ? Failure::Timeout : Failure::Connect;
}

StatusClass classifyStatus(const int status) {
  if (status == 200) return StatusClass::Ok;
  if (status == 401 || status == 403) return StatusClass::Unauthorized;
  if (status == 404) return StatusClass::NotFound;
  if (status == 410) return StatusClass::Gone;
  if (status >= 300 && status <= 399) return StatusClass::Redirect;
  if (status >= 500 && status <= 599) return StatusClass::ServerError;
  return StatusClass::Other;
}

namespace {
bool alpha(const char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool digit(const char c) { return c >= '0' && c <= '9'; }
bool hex(const char c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
char lower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
}  // namespace

bool locationHost(std::string_view location, char* out, const size_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = '\0';
  while (!location.empty() && (location.front() == ' ' || location.front() == '\t')) location.remove_prefix(1);

  // "scheme://authority..." or "//authority...": anything else is a reference relative to the request's own host.
  std::string_view rest;
  if (location.substr(0, 2) == "//") {
    rest = location.substr(2);
  } else {
    const size_t colon = location.find(':');
    if (colon == std::string_view::npos || colon == 0 || location.substr(colon, 3) != "://") return false;
    if (!alpha(location[0])) return false;
    for (size_t i = 1; i < colon; ++i) {
      const char c = location[i];
      if (!alpha(c) && !digit(c) && c != '+' && c != '-' && c != '.') return false;
    }
    rest = location.substr(colon + 3);
  }

  std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
  // Credentials in the authority are never reported.
  const size_t at = authority.rfind('@');
  if (at != std::string_view::npos) authority.remove_prefix(at + 1);

  std::string_view host;
  if (!authority.empty() && authority.front() == '[') {
    const size_t close = authority.find(']');
    if (close == std::string_view::npos) return false;
    host = authority.substr(0, close + 1);
    for (size_t i = 1; i + 1 < host.size(); ++i)
      if (!hex(host[i]) && host[i] != ':' && host[i] != '.') return false;
    if (host.size() < 3) return false;
  } else {
    host = authority.substr(0, authority.find(':'));
    if (host.empty() || host.size() > 253) return false;
    for (const char c : host)
      if (!alpha(c) && !digit(c) && c != '-' && c != '.') return false;
  }
  if (host.size() + 1 > outSize) return false;
  for (size_t i = 0; i < host.size(); ++i) out[i] = lower(host[i]);
  out[host.size()] = '\0';
  return true;
}

}  // namespace httpfail
