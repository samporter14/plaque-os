#pragma once

#include <string>
#include <utility>
#include <vector>

#include "NetJson.h"
#include "TestPki.h"

// Bundles for the tests: a valid open profile to start from, with fields set, replaced or dropped as raw JSON.
namespace testbundle {

inline std::string q(const std::string& text) {
  std::string out;
  netjson::appendString(out, text);
  return out;
}

class Bundle {
  std::vector<std::pair<std::string, std::string>> fields_;

 public:
  Bundle& set(const std::string& key, const std::string& raw) {
    for (auto& field : fields_)
      if (field.first == key) {
        field.second = raw;
        return *this;
      }
    fields_.emplace_back(key, raw);
    return *this;
  }
  Bundle& drop(const std::string& key) {
    for (auto it = fields_.begin(); it != fields_.end(); ++it)
      if (it->first == key) {
        fields_.erase(it);
        break;
      }
    return *this;
  }
  std::string text() const {
    std::string out = "{";
    for (size_t i = 0; i < fields_.size(); ++i) {
      if (i) out += ", ";
      out += q(fields_[i].first) + ": " + fields_[i].second;
    }
    return out + "}";
  }
};

inline Bundle openBundle() {
  Bundle b;
  b.set("kind", q("plaque-network-profile"))
      .set("schema", "1")
      .set("profile_id", q("5f2c9a10"))
      .set("name", q("Lab 100 Wi-Fi"))
      .set("institution", q("umich-its"))
      .set("category", q("registered-device"))
      .set("ssid", q("MSetup"))
      .set("hidden", "false")
      .set("auth", q("open"))
      .set("psk", "null")
      .set("eap", "null")
      .set("time_servers", "[]")
      .set("use_dhcp_time", "false")
      .set("created_at", "1792000000");
  return b;
}

inline const char* PSK = "correct-horse-battery-staple-42";

inline Bundle pskBundle() {
  return openBundle().set("auth", q("psk")).set("psk", q(PSK)).set("category", q("shared-password"));
}

inline std::string eapObject(const std::string& identity, const std::string& domain, const std::string& ca,
                             const std::string& cert, const std::string& key) {
  return "{" + q("identity") + ": " + q(identity) + ", " + q("server_domain") + ": " + q(domain) + ", " + q("ca_pem") +
         ": " + q(ca) + ", " + q("client_cert_pem") + ": " + q(cert) + ", " + q("client_key_pem") + ": " + q(key) + "}";
}

// The synthetic PKI every EAP-TLS test shares (made once: RSA key generation takes a moment).
struct Pki {
  testpki::Key rootKey, root2Key, root3Key, root4Key;
  testpki::Key clientRsa, otherRsa, smallRsa, clientEc256, clientEc384, clientK1;
  std::string root, root2, root3, root4, notCa, noConstraints;
  std::string certRsa, certEc256, certEc384, certSmall, certK1, certExpired, certFuture;

  Pki() {
    using testpki::CertSpec;
    testpki::makeEc(rootKey, MBEDTLS_ECP_DP_SECP256R1);
    testpki::makeEc(root2Key, MBEDTLS_ECP_DP_SECP256R1);
    testpki::makeEc(root3Key, MBEDTLS_ECP_DP_SECP384R1);
    testpki::makeEc(root4Key, MBEDTLS_ECP_DP_SECP256R1);
    testpki::makeRsa(clientRsa, 2048);
    testpki::makeRsa(otherRsa, 2048);
    testpki::makeRsa(smallRsa, 1024);
    testpki::makeEc(clientEc256, MBEDTLS_ECP_DP_SECP256R1);
    testpki::makeEc(clientEc384, MBEDTLS_ECP_DP_SECP384R1);
    testpki::makeEc(clientK1, MBEDTLS_ECP_DP_SECP256K1);
    const auto rootSpec = [](const char* name) {
      CertSpec s;
      s.subject = s.issuer = std::string("CN=") + name + ",O=Synthetic Test CA";
      s.ca = true;
      return s;
    };
    root = testpki::certPem(rootSpec("Synthetic RADIUS Root"), rootKey, rootKey);
    root2 = testpki::certPem(rootSpec("Synthetic RADIUS Root 2"), root2Key, root2Key);
    root3 = testpki::certPem(rootSpec("Synthetic RADIUS Root 3"), root3Key, root3Key);
    root4 = testpki::certPem(rootSpec("Synthetic RADIUS Root 4"), root4Key, root4Key);
    CertSpec leaf;
    leaf.subject = "CN=Synthetic leaf,O=Synthetic";
    leaf.issuer = "CN=Synthetic RADIUS Root,O=Synthetic Test CA";
    notCa = testpki::certPem(leaf, root2Key, rootKey);
    leaf.basicConstraints = false;
    noConstraints = testpki::certPem(leaf, root2Key, rootKey);
    CertSpec client;
    client.subject = "CN=plaque-0b6c9f9e,O=Synthetic University";
    client.issuer = "CN=Synthetic RADIUS Root,O=Synthetic Test CA";
    client.notBefore = "20260101000000";
    client.notAfter = "20270101000000";
    certRsa = testpki::certPem(client, clientRsa, rootKey);
    certEc256 = testpki::certPem(client, clientEc256, rootKey);
    certEc384 = testpki::certPem(client, clientEc384, rootKey);
    certSmall = testpki::certPem(client, smallRsa, rootKey);
    certK1 = testpki::certPem(client, clientK1, rootKey);
    CertSpec expired = client;
    expired.notBefore = "20250101000000";
    expired.notAfter = "20250601000000";
    certExpired = testpki::certPem(expired, clientRsa, rootKey);
    CertSpec future = client;
    future.notBefore = "20300101000000";
    future.notAfter = "20310101000000";
    certFuture = testpki::certPem(future, clientRsa, rootKey);
  }
};

inline Pki& pki() {
  static Pki shared;
  return shared;
}

inline Bundle eapBundle(const std::string& cert, const std::string& key, const std::string& ca = pki().root,
                        const std::string& domain = "radius.example.test",
                        const std::string& identity = "plaque-0b6c9f9e@example.test") {
  return openBundle()
      .set("auth", q("eap-tls"))
      .set("category", q("enterprise-certificate"))
      .set("ssid", q("MWireless"))
      .set("eap", eapObject(identity, domain, ca, cert, key));
}

}  // namespace testbundle
