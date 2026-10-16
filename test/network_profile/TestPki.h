#pragma once

#include <mbedtls/ecp.h>
#include <mbedtls/pk.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Synthetic keys and certificates, made with Mbed TLS while the tests run (deterministically, from a fixed seed).
// Nothing here is a real credential and nothing is written to disk.
namespace testpki {

// The deterministic random source the tests hand to the validator and to the generators.
int random(void* context, unsigned char* out, size_t size);
void* randomContext();

class Key {
 public:
  mbedtls_pk_context pk;
  Key();
  ~Key();
  Key(const Key&) = delete;
  Key& operator=(const Key&) = delete;
};

void makeRsa(Key& key, unsigned bits);
void makeEc(Key& key, mbedtls_ecp_group_id curve);

// PEM encodings of a private key: PKCS#1 ("RSA PRIVATE KEY") or SEC1 ("EC PRIVATE KEY") as Mbed TLS writes them, and
// PKCS#8 ("PRIVATE KEY"), wrapped here.
std::string keyPem(const Key& key);
std::string pkcs8Pem(const Key& key);
// The DER behind keyPem().
std::string keyDer(const Key& key);

struct CertSpec {
  std::string subject;  // "CN=..."
  std::string issuer;   // "CN=..."
  bool ca = false;
  bool basicConstraints = true;  // false: no basicConstraints extension at all
  std::string notBefore = "20250101000000";
  std::string notAfter = "20350101000000";
};

std::string certPem(const CertSpec& spec, Key& subjectKey, Key& issuerKey);

// PEM armour of arbitrary bytes under a label.
std::string armour(const std::string& label, const std::string& der, const std::string& headers = "");

}  // namespace testpki
