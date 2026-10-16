#include <gtest/gtest.h>

#include <string>

#include "NetworkProfile.h"
#include "TestBundles.h"
#include "TestPki.h"

using netprofile::Auth;
using netprofile::Error;
using testbundle::eapObject;
using testbundle::openBundle;
using testbundle::pki;
using testbundle::pskBundle;
using testbundle::q;

namespace {

netprofile::ValidateOptions options(const int64_t trustedNow = 0) {
  netprofile::ValidateOptions o;
  o.trustedNow = trustedNow;
  o.random = testpki::random;
  o.randomContext = testpki::randomContext();
  return o;
}

constexpr int64_t NOW = 1792000000;  // October 2026: inside the synthetic client certificate's validity

Error parse(const testbundle::Bundle& bundle, const int64_t trustedNow = 0) {
  netprofile::Profile profile;
  return netprofile::parseBundle(bundle.text(), options(trustedNow), profile);
}

testbundle::Bundle eapBundle(const std::string& ca, const std::string& cert, const std::string& key,
                             const std::string& domain = "radius.example.edu") {
  return openBundle()
      .set("auth", q("eap-tls"))
      .set("category", q("enterprise-certificate"))
      .set("eap", eapObject("plaque-0b6c9f9e@example.edu", domain, ca, cert, key));
}

testbundle::Bundle validEap() { return eapBundle(pki().root, pki().certRsa, testpki::keyPem(pki().clientRsa)); }

}  // namespace

TEST(NetworkProfile, AnOpenProfileValidatesAndKeepsItsPublicFields) {
  netprofile::Profile profile;
  ASSERT_EQ(Error::None, netprofile::parseBundle(openBundle().text(), options(), profile));
  EXPECT_EQ("Lab 100 Wi-Fi", profile.info.name);
  EXPECT_EQ("MSetup", profile.info.ssid);
  EXPECT_EQ(Auth::Open, profile.info.auth);
  EXPECT_EQ("5f2c9a10", profile.info.profileId);
  EXPECT_TRUE(profile.info.timeServers.empty());
}

TEST(NetworkProfile, PskRules) {
  netprofile::Profile profile;
  ASSERT_EQ(Error::None, netprofile::parseBundle(pskBundle().text(), options(), profile));
  EXPECT_EQ(Auth::Psk, profile.info.auth);
  EXPECT_EQ(std::string(testbundle::PSK).size(), profile.psk.size());

  EXPECT_EQ(Error::None, parse(pskBundle().set("psk", q("12345678"))));                  // 8: the shortest
  EXPECT_EQ(Error::None, parse(pskBundle().set("psk", q(std::string(63, 'a')))));        // 63: the longest
  EXPECT_EQ(Error::None, parse(pskBundle().set("psk", q(std::string(64, 'A')))));        // 64 hex digits
  EXPECT_EQ(Error::PskInvalid, parse(pskBundle().set("psk", q("1234567"))));             // 7
  EXPECT_EQ(Error::PskInvalid, parse(pskBundle().set("psk", q(std::string(64, 'g')))));  // 64, not hex
  EXPECT_EQ(Error::PskInvalid, parse(pskBundle().set("psk", q(std::string(65, 'a')))));  // 65
  EXPECT_EQ(Error::PskInvalid, parse(pskBundle().set("psk", q("pass\x01word1"))));       // a control character
  EXPECT_EQ(Error::PskInvalid, parse(pskBundle().set("psk", "null")));
  EXPECT_EQ(Error::PskInvalid, parse(pskBundle().drop("psk")));
}

TEST(NetworkProfile, AnOpenOrCertificateProfileMayNotCarryAPsk) {
  EXPECT_NE(Error::None, parse(openBundle().set("psk", q("12345678"))));
  EXPECT_NE(Error::None, parse(validEap().set("psk", q("12345678"))));
}

TEST(NetworkProfile, AnEapObjectOnAnotherAuthIsRefused) {
  EXPECT_NE(Error::None, parse(openBundle().set("eap", eapObject("a@b.edu", "radius.example.edu", pki().root,
                                                                 pki().certRsa, testpki::keyPem(pki().clientRsa)))));
}

TEST(NetworkProfile, TheEnvelopeIsChecked) {
  netprofile::Profile profile;
  EXPECT_EQ(Error::BadJson, netprofile::parseBundle("not json", options(), profile));
  EXPECT_EQ(Error::BadJson, netprofile::parseBundle("", options(), profile));
  EXPECT_EQ(Error::BadKind,
            netprofile::parseBundle(openBundle().set("kind", q("plaque-other")).text(), options(), profile));
  EXPECT_EQ(Error::BadSchema, netprofile::parseBundle(openBundle().set("schema", "2").text(), options(), profile));
  EXPECT_EQ(Error::BadSchema, netprofile::parseBundle(openBundle().set("schema", "0").text(), options(), profile));
  EXPECT_EQ(Error::BadField, netprofile::parseBundle(openBundle().set("surprise", "1").text(), options(), profile));
  EXPECT_EQ(Error::TooLarge,
            netprofile::parseBundle(std::string(netprofile::MAX_BUNDLE_BYTES + 1, ' '), options(), profile));
}

TEST(NetworkProfile, NameRules) {
  EXPECT_EQ(Error::None, parse(openBundle().set("name", q(std::string(40, 'n')))));
  EXPECT_EQ(Error::NameInvalid, parse(openBundle().set("name", q(""))));
  EXPECT_EQ(Error::NameInvalid, parse(openBundle().set("name", q(std::string(41, 'n')))));
  EXPECT_EQ(Error::NameInvalid, parse(openBundle().set("name", q("two\nlines"))));
  EXPECT_EQ(Error::NameInvalid,
            parse(openBundle().set("name", q("zero\xE2\x80\x8Bwidth"))));  // U+200B, a format character
  EXPECT_EQ(Error::NameInvalid, parse(openBundle().set("name", q("bidi\xE2\x80\xAE"
                                                                 "x"))));  // U+202E
  EXPECT_EQ(Error::NameInvalid, parse(openBundle().set("name", q("sep\xE2\x80\xA8"
                                                                 "x"))));  // U+2028
}

TEST(NetworkProfile, SsidRules) {
  EXPECT_EQ(Error::None, parse(openBundle().set("ssid", q(std::string(32, 's')))));
  EXPECT_EQ(Error::SsidInvalid, parse(openBundle().set("ssid", q(""))));
  EXPECT_EQ(Error::SsidInvalid, parse(openBundle().set("ssid", q(std::string(33, 's')))));
  EXPECT_EQ(Error::SsidInvalid, parse(openBundle().set("ssid", q("tab\there"))));
  // The limit is in bytes: 17 two-byte characters are 34 bytes.
  std::string wide;
  for (int i = 0; i < 17; ++i) wide += "\xC3\xA9";
  EXPECT_EQ(Error::SsidInvalid, parse(openBundle().set("ssid", q(wide))));
}

TEST(NetworkProfile, OnlyOpenPskAndEapTlsExist) {
  EXPECT_EQ(Error::AuthUnsupported, parse(openBundle().set("auth", q("peap"))));
  EXPECT_EQ(Error::AuthUnsupported, parse(openBundle().set("auth", q("eap-ttls"))));
  EXPECT_EQ(Error::AuthUnsupported, parse(openBundle().set("auth", q("wpa2-enterprise"))));
}

TEST(NetworkProfile, TimeServers) {
  netprofile::Profile profile;
  ASSERT_EQ(Error::None,
            netprofile::parseBundle(
                openBundle().set("time_servers", "[\"ntp.example.edu\", \"10.1.2.3\", \"time2.example.edu\"]").text(),
                options(), profile));
  ASSERT_EQ(3u, profile.info.timeServers.size());
  EXPECT_EQ("ntp.example.edu", profile.info.timeServers[0]);
  EXPECT_EQ(Error::TimeServerInvalid,
            parse(openBundle().set("time_servers", "[\"a.edu\",\"b.edu\",\"c.edu\",\"d.edu\"]")));
  EXPECT_EQ(Error::TimeServerInvalid, parse(openBundle().set("time_servers", "[\"bad host!\"]")));
  EXPECT_EQ(Error::TimeServerInvalid, parse(openBundle().set("time_servers", "[\"-leading.example\"]")));
  EXPECT_EQ(Error::TimeServerInvalid, parse(openBundle().set("time_servers", "[\"\"]")));
  EXPECT_TRUE(netprofile::validHostname("ntp.example.edu"));
  EXPECT_FALSE(netprofile::validHostname("under_score.example"));
  EXPECT_TRUE(netprofile::validIpv4("10.0.0.53"));
  EXPECT_FALSE(netprofile::validIpv4("10.0.0.256"));
}

TEST(NetworkProfile, AValidCertificateProfileInRsaAndEc) {
  netprofile::Profile profile;
  ASSERT_EQ(Error::None, netprofile::parseBundle(validEap().text(), options(), profile));
  EXPECT_EQ(Auth::EapTls, profile.info.auth);
  EXPECT_EQ("radius.example.edu", profile.info.serverDomain);
  EXPECT_EQ(1u, profile.info.caSha256.size());
  EXPECT_NE(std::string::npos, profile.info.clientCertSubject.find("plaque-0b6c9f9e"));
  EXPECT_TRUE(profile.info.hasCertDates);
  EXPECT_FALSE(profile.clientCertDer.empty());

  EXPECT_EQ(Error::None, parse(eapBundle(pki().root, pki().certEc256, testpki::keyPem(pki().clientEc256))));
  EXPECT_EQ(Error::None, parse(eapBundle(pki().root, pki().certEc384, testpki::keyPem(pki().clientEc384))));
  EXPECT_EQ(Error::None, parse(eapBundle(pki().root, pki().certRsa, testpki::pkcs8Pem(pki().clientRsa))));
  EXPECT_EQ(Error::None, parse(eapBundle(pki().root, pki().certEc256, testpki::pkcs8Pem(pki().clientEc256))));
}

TEST(NetworkProfile, UpToThreeCaCertificates) {
  EXPECT_EQ(Error::None,
            parse(eapBundle(pki().root + pki().root2 + pki().root3, pki().certRsa, testpki::keyPem(pki().clientRsa))));
  EXPECT_EQ(Error::CaInvalid, parse(eapBundle(pki().root + pki().root2 + pki().root3 + pki().root4, pki().certRsa,
                                              testpki::keyPem(pki().clientRsa))));
  EXPECT_EQ(Error::CaInvalid, parse(eapBundle("", pki().certRsa, testpki::keyPem(pki().clientRsa))));
}

TEST(NetworkProfile, ACertificateThatIsNotACaIsRefusedAsOne) {
  EXPECT_EQ(Error::CaInvalid, parse(eapBundle(pki().notCa, pki().certRsa, testpki::keyPem(pki().clientRsa))));
  EXPECT_EQ(Error::CaInvalid, parse(eapBundle(pki().noConstraints, pki().certRsa, testpki::keyPem(pki().clientRsa))));
  EXPECT_EQ(Error::CaInvalid,
            parse(eapBundle(pki().root + pki().notCa, pki().certRsa, testpki::keyPem(pki().clientRsa))));
}

TEST(NetworkProfile, TheKeyMustMatchTheCertificate) {
  EXPECT_EQ(Error::KeyMismatch, parse(eapBundle(pki().root, pki().certRsa, testpki::keyPem(pki().otherRsa))));
  EXPECT_EQ(Error::KeyMismatch, parse(eapBundle(pki().root, pki().certRsa, testpki::keyPem(pki().clientEc256))));
  EXPECT_EQ(Error::KeyMismatch, parse(eapBundle(pki().root, pki().certEc256, testpki::keyPem(pki().clientEc384))));
}

TEST(NetworkProfile, EncryptedKeysAreRefusedNotGuessedAt) {
  const std::string der = testpki::keyDer(pki().clientRsa);
  const std::string legacy = testpki::armour(
      "RSA PRIVATE KEY", der, "Proc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,00112233445566778899AABBCCDDEEFF\n\n");
  EXPECT_EQ(Error::KeyEncrypted, parse(eapBundle(pki().root, pki().certRsa, legacy)));
  const std::string pkcs8 = testpki::armour("ENCRYPTED PRIVATE KEY", der);
  EXPECT_EQ(Error::KeyEncrypted, parse(eapBundle(pki().root, pki().certRsa, pkcs8)));
}

TEST(NetworkProfile, WeakOrUnsupportedKeysAreRefused) {
  EXPECT_EQ(Error::ClientKeyInvalid, parse(eapBundle(pki().root, pki().certSmall, testpki::keyPem(pki().smallRsa))));
  EXPECT_EQ(Error::ClientKeyInvalid, parse(eapBundle(pki().root, pki().certK1, testpki::keyPem(pki().clientK1))));
  EXPECT_EQ(
      Error::ClientKeyInvalid,
      parse(eapBundle(pki().root, pki().certRsa, "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n")));
  EXPECT_EQ(Error::ClientKeyInvalid, parse(eapBundle(pki().root, pki().certRsa, "")));
}

TEST(NetworkProfile, TheClientCertificateIsOneValidCertificate) {
  EXPECT_EQ(Error::ClientCertInvalid, parse(eapBundle(pki().root, "", testpki::keyPem(pki().clientRsa))));
  EXPECT_EQ(Error::ClientCertInvalid,
            parse(eapBundle(pki().root, pki().certRsa + pki().certRsa, testpki::keyPem(pki().clientRsa))));
  EXPECT_EQ(Error::ClientCertInvalid,
            parse(eapBundle(pki().root, "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n",
                            testpki::keyPem(pki().clientRsa))));
}

TEST(NetworkProfile, TheServerNameAndIdentityAreRequired) {
  EXPECT_EQ(Error::ServerDomainInvalid,
            parse(eapBundle(pki().root, pki().certRsa, testpki::keyPem(pki().clientRsa), "")));
  EXPECT_EQ(Error::ServerDomainInvalid,
            parse(eapBundle(pki().root, pki().certRsa, testpki::keyPem(pki().clientRsa), "not a host")));
  EXPECT_EQ(Error::ServerDomainInvalid,
            parse(eapBundle(pki().root, pki().certRsa, testpki::keyPem(pki().clientRsa), std::string(254, 'a'))));
  const auto noIdentity =
      openBundle()
          .set("auth", q("eap-tls"))
          .set("eap", eapObject("", "radius.example.edu", pki().root, pki().certRsa, testpki::keyPem(pki().clientRsa)));
  EXPECT_EQ(Error::EapIdentityInvalid, parse(noIdentity));
  const auto longIdentity =
      openBundle()
          .set("auth", q("eap-tls"))
          .set("eap", eapObject(std::string(netprofile::MAX_IDENTITY_BYTES + 1, 'i'), "radius.example.edu", pki().root,
                                pki().certRsa, testpki::keyPem(pki().clientRsa)));
  EXPECT_EQ(Error::EapIdentityInvalid, parse(longIdentity));
  const auto noEap = openBundle().set("auth", q("eap-tls")).set("eap", "null");
  EXPECT_NE(Error::None, parse(noEap));
}

TEST(NetworkProfile, CertificateDatesAreJudgedOnlyWithTrustedTime) {
  const auto expired = eapBundle(pki().root, pki().certExpired, testpki::keyPem(pki().clientRsa));
  const auto future = eapBundle(pki().root, pki().certFuture, testpki::keyPem(pki().clientRsa));
  EXPECT_EQ(Error::ClientCertExpired, parse(expired, NOW));
  EXPECT_EQ(Error::ClientCertNotYetValid, parse(future, NOW));
  EXPECT_EQ(Error::None, parse(validEap(), NOW));
  // Without trusted time the dates are carried, never judged.
  netprofile::Profile profile;
  EXPECT_EQ(Error::None, netprofile::parseBundle(expired.text(), options(0), profile));
  EXPECT_TRUE(profile.info.hasCertDates);
  EXPECT_LT(profile.info.clientCertNotAfter, NOW);
}

TEST(NetworkProfile, TheCardCarriesNoPrivateKeys) {
  netprofile::Profile profile;
  auto o = options();
  o.source = netprofile::Source::Card;
  EXPECT_EQ(Error::CardKeyRefused, netprofile::parseCardBundle(validEap().text(), o, profile));
  // Refused before anything else is looked at: even a bundle that is otherwise broken.
  EXPECT_EQ(Error::CardKeyRefused,
            netprofile::parseCardBundle(
                eapBundle("junk", "junk", "-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n").text(), o,
                profile));
  EXPECT_EQ(Error::None, netprofile::parseCardBundle(openBundle().text(), o, profile));
  EXPECT_EQ(Error::None, netprofile::parseCardBundle(pskBundle().text(), o, profile));
  EXPECT_EQ(netprofile::Source::Card, profile.info.installedVia);
}

TEST(NetworkProfile, NoSecretReachesTheSummaryTheImportResultOrAnErrorCode) {
  netprofile::Profile psk;
  ASSERT_EQ(Error::None, netprofile::parseBundle(pskBundle().text(), options(), psk));
  const std::string pskSummary = netprofile::summaryJson(psk.info);
  EXPECT_EQ(std::string::npos, pskSummary.find(testbundle::PSK));
  EXPECT_NE(std::string::npos, pskSummary.find("MSetup"));
  EXPECT_EQ(std::string::npos, netprofile::importResultJson(NOW, Error::None, &psk.info).find(testbundle::PSK));

  netprofile::Profile eap;
  ASSERT_EQ(Error::None, netprofile::parseBundle(validEap().text(), options(), eap));
  const std::string key = testpki::keyPem(pki().clientRsa);
  const std::string body = key.substr(key.find('\n') + 1, 40);  // a stretch of the key's base64
  const std::string summary = netprofile::summaryJson(eap.info);
  EXPECT_EQ(std::string::npos, summary.find("PRIVATE KEY"));
  EXPECT_EQ(std::string::npos, summary.find(body));
  EXPECT_NE(std::string::npos, summary.find("radius.example.edu"));
  EXPECT_NE(std::string::npos, summary.find("ca_sha256"));
  const std::string result = netprofile::importResultJson(0, Error::KeyMismatch, &eap.info);
  EXPECT_EQ(std::string::npos, result.find(body));
  EXPECT_NE(std::string::npos, result.find("key_mismatch"));
  EXPECT_NE(std::string::npos, result.find("\"at\":null"));
  for (int e = 0; e <= static_cast<int>(Error::Busy); ++e) {
    const std::string code = netprofile::errorCode(static_cast<Error>(e));
    EXPECT_EQ(std::string::npos, code.find(testbundle::PSK));
    EXPECT_FALSE(code.empty());
  }
}

TEST(NetworkProfile, TheSummaryWithNothingInstalled) {
  EXPECT_EQ("{\"schema\":1,\"installed\":false}\n", netprofile::summaryJson(netprofile::Summary()));
}

TEST(NetworkProfile, DatesAndDigests) {
  EXPECT_EQ(0, netprofile::unixFromUtc(1970, 1, 1, 0, 0, 0));
  EXPECT_EQ(1792000000, netprofile::unixFromUtc(2026, 10, 14, 17, 46, 40));
  EXPECT_EQ("2026-10-14", netprofile::isoDate(1792000000));
  netprofile::Digest d;
  d.bytes[0] = 0xab;
  d.bytes[31] = 0x01;
  const std::string hex = netprofile::digestHex(d);
  EXPECT_EQ(64u, hex.size());
  EXPECT_EQ("ab", hex.substr(0, 2));
  EXPECT_EQ("01", hex.substr(62, 2));
}

// ---- drawing an SSID: invisible and direction-changing characters are not drawn
// --------------------------------------

TEST(NetworkProfileDisplay, OrdinaryTextIsDrawnAsItIs) {
  EXPECT_EQ("MSetup", netprofile::displaySafe("MSetup"));
  EXPECT_EQ("", netprofile::displaySafe(""));
  EXPECT_EQ("Caf\xC3\xA9 \xCE\x94 \xE6\x97\xA5\xE6\x9C\xAC",
            netprofile::displaySafe("Caf\xC3\xA9 \xCE\x94 \xE6\x97\xA5\xE6\x9C\xAC"));
  EXPECT_EQ("lab \xF0\x9F\x98\x80", netprofile::displaySafe("lab \xF0\x9F\x98\x80"));  // four bytes: U+1F600
}

TEST(NetworkProfileDisplay, FormatAndSeparatorCharactersBecomeTheReplacementCharacter) {
  const std::string fffd = "\xEF\xBF\xBD";
  // U+202E, the right-to-left override: "MSetup" would read backwards after it.
  EXPECT_EQ("Lab" + fffd + "WiFi", netprofile::displaySafe("Lab\xE2\x80\xAEWiFi"));
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xE2\x80\x8B"
                                                      "b"));  // U+200B zero-width space
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xE2\x80\x8D"
                                                      "b"));  // U+200D zero-width joiner
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xE2\x80\xA8"
                                                      "b"));  // U+2028 line separator
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xE2\x80\xA9"
                                                      "b"));  // U+2029 paragraph separator
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xEF\xBB\xBF"
                                                      "b"));  // U+FEFF
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xC2\xAD"
                                                      "b"));  // U+00AD soft hyphen
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xE2\x81\xA6"
                                                      "b"));  // U+2066 isolate
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xF3\xA0\x80\x81"
                                                      "b"));  // U+E0001 language tag
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\x01"
                                                      "b"));  // a control, though validation keeps them out
  EXPECT_EQ(fffd + fffd + fffd, netprofile::displaySafe("\xE2\x80\xAE\xE2\x80\xAE\xE2\x80\xAE"));
}

TEST(NetworkProfileDisplay, BytesThatAreNotUtf8AreReplacedToo) {
  const std::string fffd = "\xEF\xBF\xBD";
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\xFF"
                                                      "b"));
  EXPECT_EQ("a" + fffd + "b", netprofile::displaySafe("a\x80"
                                                      "b"));               // a continuation byte alone
  EXPECT_EQ("a" + fffd + fffd, netprofile::displaySafe("a\xE2\x80"));      // cut short: each byte is replaced
  EXPECT_EQ(fffd + fffd, netprofile::displaySafe("\xC0\x80"));             // overlong NUL
  EXPECT_EQ(fffd + fffd + fffd, netprofile::displaySafe("\xED\xA0\x80"));  // a surrogate
  EXPECT_EQ(fffd + fffd + fffd + fffd, netprofile::displaySafe("\xF4\x90\x80\x80"));  // past U+10FFFF
}

TEST(NetworkProfileDisplay, ValidationOfTheSsidIsUnchangedAndTheStoredValueIsTheOriginal) {
  // The Mac agrees with this validation, so it must not move: a name with a format character is still a valid SSID.
  const std::string odd = "Lab\xE2\x80\xAEWiFi";
  netprofile::Profile profile;
  ASSERT_EQ(Error::None, netprofile::parseBundle(openBundle().set("ssid", q(odd)).text(), options(), profile));
  EXPECT_EQ(odd, profile.info.ssid);
  EXPECT_NE(odd, netprofile::displaySafe(profile.info.ssid));
  // And a profile name still may not have one.
  EXPECT_EQ(Error::NameInvalid, parse(openBundle().set("name", q(odd))));
}

// ---- a board supports some kinds of network and not others
// ---------------------------------------------------------------

TEST(NetworkProfile, AKindOfNetworkTheBoardDoesNotSupportIsRefused) {
  netprofile::ValidateOptions noPsk = options();
  noPsk.allowPsk = false;
  netprofile::Profile profile;
  EXPECT_EQ(Error::AuthUnsupported, netprofile::parseBundle(pskBundle().text(), noPsk, profile));
  EXPECT_TRUE(profile.psk.empty());  // refused before the passphrase was read
  EXPECT_EQ(Error::None, netprofile::parseBundle(openBundle().text(), noPsk, profile));

  netprofile::ValidateOptions noOpen = options();
  noOpen.allowOpen = false;
  EXPECT_EQ(Error::AuthUnsupported, netprofile::parseBundle(openBundle().text(), noOpen, profile));
  EXPECT_EQ(Error::None, netprofile::parseBundle(pskBundle().text(), noOpen, profile));

  netprofile::ValidateOptions noEap = options();
  noEap.allowEapTls = false;
  netprofile::Profile eap;
  EXPECT_EQ(Error::AuthUnsupported, netprofile::parseBundle(validEap().text(), noEap, eap));
  EXPECT_TRUE(eap.keyDer.empty());  // refused before the key was read
  EXPECT_EQ(Error::None, netprofile::parseBundle(validEap().text(), options(), eap));

  // The card inbox goes through the same rule.
  EXPECT_EQ(Error::AuthUnsupported, netprofile::parseCardBundle(pskBundle().text(), noPsk, profile));
  EXPECT_EQ(Error::CardKeyRefused, netprofile::parseCardBundle(validEap().text(), options(), eap));
}
