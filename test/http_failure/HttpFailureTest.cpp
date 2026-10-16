#include <gtest/gtest.h>

#include <string>

#include "network/HttpFailure.h"

using httpfail::classifyHeaderRead;
using httpfail::classifyStatus;
using httpfail::classifyTransport;
using httpfail::Failure;
using httpfail::StatusClass;
using httpfail::TransportError;

namespace {
TransportError tls(int tlsError, int code = 0, int flags = 0, int sockErrno = 0) {
  TransportError e;
  e.tlsError = tlsError;
  e.tlsCode = code;
  e.certFlags = flags;
  e.socketErrno = sockErrno;
  return e;
}

std::string host(const char* location) {
  char out[254];
  if (!httpfail::locationHost(location, out, sizeof(out))) return "<none>";
  return out;
}

constexpr int BADCERT_EXPIRED = 0x01;
constexpr int BADCERT_CN_MISMATCH = 0x04;
constexpr int BADCERT_NOT_TRUSTED = 0x08;
constexpr int SSL_FATAL_ALERT = 0x7780;  // MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE, magnitude
constexpr int NET_RECV_FAILED = 0x004C;  // MBEDTLS_ERR_NET_RECV_FAILED, magnitude
constexpr int REFUSED_ERRNO = 111;       // ECONNREFUSED in newlib
}  // namespace

TEST(HttpFailure, NameLookupAndConnectionFailures) {
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_CANNOT_RESOLVE_HOSTNAME)), Failure::Connect);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_FAILED_CONNECT_TO_HOST, 0, 0, REFUSED_ERRNO)), Failure::Connect);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_CANNOT_CREATE_SOCKET)), Failure::Connect);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_UNSUPPORTED_PROTOCOL_FAMILY)), Failure::Connect);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_SOCKET_SETOPT_FAILED)), Failure::Connect);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_TCP_CLOSED_FIN)), Failure::Connect);
  EXPECT_EQ(classifyTransport(tls(0)), Failure::Connect);  // nothing reported: still a failed connection
}

TEST(HttpFailure, Timeouts) {
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_CONNECTION_TIMEOUT)), Failure::Timeout);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_SERVER_HANDSHAKE_TIMEOUT)), Failure::Timeout);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_FAILED_CONNECT_TO_HOST, 0, 0, httpfail::ERRNO_ETIMEDOUT)),
            Failure::Timeout);
  EXPECT_EQ(
      classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, NET_RECV_FAILED, 0, httpfail::ERRNO_EAGAIN)),
      Failure::Timeout);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, httpfail::MBEDTLS_SSL_TIMEOUT)),
            Failure::Timeout);
  EXPECT_EQ(classifyTransport(tls(0, 0, 0, httpfail::ERRNO_EAGAIN)), Failure::Timeout);
}

TEST(HttpFailure, CertificateVerification) {
  // ESP-TLS stores the magnitude of the mbedTLS code; either sign is read.
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, httpfail::MBEDTLS_CERT_VERIFY_FAILED)),
            Failure::Certificate);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, -httpfail::MBEDTLS_CERT_VERIFY_FAILED)),
            Failure::Certificate);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, 0, BADCERT_EXPIRED)),
            Failure::Certificate);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, 0, BADCERT_CN_MISMATCH)),
            Failure::Certificate);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, 0, BADCERT_NOT_TRUSTED)),
            Failure::Certificate);
  // Flags win over a timeout errno: verification ran and refused.
  EXPECT_EQ(
      classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, 0, BADCERT_EXPIRED, httpfail::ERRNO_EAGAIN)),
      Failure::Certificate);
}

TEST(HttpFailure, OtherHandshakeFailures) {
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED, SSL_FATAL_ALERT)), Failure::Tls);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_FIRST)), Failure::Tls);
  EXPECT_EQ(classifyTransport(tls(httpfail::TLS_ERR_MBEDTLS_LAST)), Failure::Tls);
}

TEST(HttpFailure, HeaderRead) {
  EXPECT_EQ(classifyHeaderRead(-httpfail::HTTP_ERR_EAGAIN), Failure::Timeout);
  EXPECT_EQ(classifyHeaderRead(-1), Failure::Connect);
}

TEST(HttpFailure, StatusClasses) {
  EXPECT_EQ(classifyStatus(200), StatusClass::Ok);
  EXPECT_EQ(classifyStatus(401), StatusClass::Unauthorized);
  EXPECT_EQ(classifyStatus(403), StatusClass::Unauthorized);
  EXPECT_EQ(classifyStatus(404), StatusClass::NotFound);
  EXPECT_EQ(classifyStatus(410), StatusClass::Gone);
  for (const int code : {300, 301, 302, 303, 307, 308, 399}) EXPECT_EQ(classifyStatus(code), StatusClass::Redirect);
  for (const int code : {500, 502, 503, 599}) EXPECT_EQ(classifyStatus(code), StatusClass::ServerError);
  for (const int code : {0, 201, 204, 206, 400, 405, 429, 600}) EXPECT_EQ(classifyStatus(code), StatusClass::Other);
}

TEST(HttpFailure, LocationHostIsTheHostOnly) {
  EXPECT_EQ(host("https://portal.example.edu/login?next=/api"), "portal.example.edu");
  EXPECT_EQ(host("http://Portal.Example.EDU:8080/x"), "portal.example.edu");
  EXPECT_EQ(host("https://portal.example.edu"), "portal.example.edu");
  EXPECT_EQ(host("https://portal.example.edu?x=1"), "portal.example.edu");
  EXPECT_EQ(host("https://portal.example.edu#top"), "portal.example.edu");
  EXPECT_EQ(host("//cdn.example.org/a"), "cdn.example.org");
  EXPECT_EQ(host("  https://10.0.0.1/login"), "10.0.0.1");
  EXPECT_EQ(host("https://[2001:db8::1]:443/x"), "[2001:db8::1]");
}

TEST(HttpFailure, LocationNeverReportsCredentials) {
  EXPECT_EQ(host("https://user:secret@portal.example.edu/"), "portal.example.edu");
  EXPECT_EQ(host("https://token@portal.example.edu:8443"), "portal.example.edu");
}

TEST(HttpFailure, RelativeOrOddLocationsNameNoHost) {
  EXPECT_EQ(host("/login"), "<none>");
  EXPECT_EQ(host("login.html"), "<none>");
  EXPECT_EQ(host(""), "<none>");
  EXPECT_EQ(host("https:///path"), "<none>");
  EXPECT_EQ(host("https://bad host/"), "<none>");
  EXPECT_EQ(host("https://bad_host.example/"), "<none>");
  EXPECT_EQ(host("https://[zz::1]/"), "<none>");
  EXPECT_EQ(host("1https://host/"), "<none>");
  EXPECT_EQ(host(("https://" + std::string(254, 'a') + "/").c_str()), "<none>");
  char small[4];
  EXPECT_FALSE(httpfail::locationHost("https://example.org/", small, sizeof(small)));
  EXPECT_STREQ(small, "");
}

TEST(HttpFailure, FailureNamesForTheLog) {
  EXPECT_STREQ(httpfail::failureName(Failure::Redirect), "redirect");
  EXPECT_STREQ(httpfail::failureName(Failure::Certificate), "certificate");
  EXPECT_STREQ(httpfail::failureName(Failure::None), "none");
}
