#include "TestPki.h"

#include <gtest/gtest.h>
#include <mbedtls/asn1write.h>
#include <mbedtls/base64.h>
#include <mbedtls/oid.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>

#include <cstring>
#include <vector>

namespace testpki {
namespace {

// SHA-256 in counter mode over a fixed seed: reproducible "randomness" for key generation and signatures.
struct Stream {
  uint64_t counter = 0;
  uint8_t block[32] = {};
  size_t used = 32;
};
Stream stream;

}  // namespace

int random(void* context, unsigned char* out, const size_t size) {
  auto* s = static_cast<Stream*>(context);
  for (size_t i = 0; i < size; ++i) {
    if (s->used == sizeof(s->block)) {
      uint8_t input[16] = {'p', 'l', 'a', 'q', 'u', 'e', '-', 't'};
      std::memcpy(input + 8, &s->counter, sizeof(s->counter));
      ++s->counter;
      mbedtls_sha256(input, sizeof(input), s->block, 0);
      s->used = 0;
    }
    out[i] = s->block[s->used++];
  }
  return 0;
}

void* randomContext() { return &stream; }

Key::Key() { mbedtls_pk_init(&pk); }
Key::~Key() { mbedtls_pk_free(&pk); }

void makeRsa(Key& key, const unsigned bits) {
  ASSERT_EQ(mbedtls_pk_setup(&key.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)), 0);
  ASSERT_EQ(mbedtls_rsa_gen_key(mbedtls_pk_rsa(key.pk), random, randomContext(), bits, 65537), 0);
}

void makeEc(Key& key, const mbedtls_ecp_group_id curve) {
  ASSERT_EQ(mbedtls_pk_setup(&key.pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)), 0);
  ASSERT_EQ(mbedtls_ecp_gen_key(curve, mbedtls_pk_ec(key.pk), random, randomContext()), 0);
}

std::string keyPem(const Key& key) {
  std::vector<unsigned char> buffer(16384);
  EXPECT_EQ(mbedtls_pk_write_key_pem(&key.pk, buffer.data(), buffer.size()), 0);
  return reinterpret_cast<const char*>(buffer.data());
}

std::string keyDer(const Key& key) {
  std::vector<unsigned char> buffer(16384);
  const int length = mbedtls_pk_write_key_der(&key.pk, buffer.data(), buffer.size());
  EXPECT_GT(length, 0);
  return std::string(reinterpret_cast<const char*>(buffer.data() + buffer.size() - length), length);
}

std::string pkcs8Pem(const Key& key) {
  const std::string inner = keyDer(key);
  std::vector<unsigned char> buffer(16384);
  unsigned char* start = buffer.data();
  unsigned char* p = start + buffer.size();
  size_t length = 0;
  length +=
      mbedtls_asn1_write_raw_buffer(&p, start, reinterpret_cast<const unsigned char*>(inner.data()), inner.size());
  length += mbedtls_asn1_write_len(&p, start, inner.size());
  length += mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_OCTET_STRING);
  if (mbedtls_pk_get_type(&key.pk) == MBEDTLS_PK_RSA) {
    length += mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_PKCS1_RSA,
                                                      MBEDTLS_OID_SIZE(MBEDTLS_OID_PKCS1_RSA), 0);
  } else {
    const char* curveOid = nullptr;
    size_t curveOidLength = 0;
    EXPECT_EQ(mbedtls_oid_get_oid_by_ec_grp(mbedtls_ecp_keypair_get_group_id(mbedtls_pk_ec(key.pk)), &curveOid,
                                            &curveOidLength),
              0);
    const int parameters = mbedtls_asn1_write_oid(&p, start, curveOid, curveOidLength);
    length += parameters;
    length += mbedtls_asn1_write_algorithm_identifier(&p, start, MBEDTLS_OID_EC_ALG_UNRESTRICTED,
                                                      MBEDTLS_OID_SIZE(MBEDTLS_OID_EC_ALG_UNRESTRICTED), parameters);
    length -= parameters;  // counted inside the identifier's length already
  }
  length += mbedtls_asn1_write_int(&p, start, 0);
  length += mbedtls_asn1_write_len(&p, start, length);
  length += mbedtls_asn1_write_tag(&p, start, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
  return armour("PRIVATE KEY", std::string(reinterpret_cast<const char*>(p), length));
}

std::string certPem(const CertSpec& spec, Key& subjectKey, Key& issuerKey) {
  static uint8_t serial = 1;
  mbedtls_x509write_cert crt;
  mbedtls_x509write_crt_init(&crt);
  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &subjectKey.pk);
  mbedtls_x509write_crt_set_issuer_key(&crt, &issuerKey.pk);
  EXPECT_EQ(mbedtls_x509write_crt_set_subject_name(&crt, spec.subject.c_str()), 0);
  EXPECT_EQ(mbedtls_x509write_crt_set_issuer_name(&crt, spec.issuer.c_str()), 0);
  unsigned char serialBytes[2] = {0x01, serial++};
  EXPECT_EQ(mbedtls_x509write_crt_set_serial_raw(&crt, serialBytes, sizeof(serialBytes)), 0);
  EXPECT_EQ(mbedtls_x509write_crt_set_validity(&crt, spec.notBefore.c_str(), spec.notAfter.c_str()), 0);
  if (spec.basicConstraints) EXPECT_EQ(mbedtls_x509write_crt_set_basic_constraints(&crt, spec.ca ? 1 : 0, -1), 0);
  std::vector<unsigned char> buffer(16384);
  EXPECT_EQ(mbedtls_x509write_crt_pem(&crt, buffer.data(), buffer.size(), random, randomContext()), 0);
  mbedtls_x509write_crt_free(&crt);
  return reinterpret_cast<const char*>(buffer.data());
}

std::string armour(const std::string& label, const std::string& der, const std::string& headers) {
  size_t length = 0;
  mbedtls_base64_encode(nullptr, 0, &length, reinterpret_cast<const unsigned char*>(der.data()), der.size());
  std::vector<unsigned char> encoded(length + 1);
  EXPECT_EQ(mbedtls_base64_encode(encoded.data(), encoded.size(), &length,
                                  reinterpret_cast<const unsigned char*>(der.data()), der.size()),
            0);
  std::string out = "-----BEGIN " + label + "-----\n" + headers;
  for (size_t i = 0; i < length; i += 64)
    out += std::string(reinterpret_cast<const char*>(encoded.data()) + i, std::min<size_t>(64, length - i)) + "\n";
  out += "-----END " + label + "-----\n";
  return out;
}

}  // namespace testpki
