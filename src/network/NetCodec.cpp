#include "NetCodec.h"

#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

namespace netcodec {
namespace {
constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sextet(const char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

bool space(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
}  // namespace

std::string base64Encode(const uint8_t* data, const size_t size) {
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
    out.push_back(ALPHABET[(v >> 18) & 63]);
    out.push_back(ALPHABET[(v >> 12) & 63]);
    out.push_back(ALPHABET[(v >> 6) & 63]);
    out.push_back(ALPHABET[v & 63]);
  }
  if (i < size) {
    uint32_t v = uint32_t(data[i]) << 16;
    if (i + 1 < size) v |= uint32_t(data[i + 1]) << 8;
    out.push_back(ALPHABET[(v >> 18) & 63]);
    out.push_back(ALPHABET[(v >> 12) & 63]);
    out.push_back(i + 1 < size ? ALPHABET[(v >> 6) & 63] : '=');
    out.push_back('=');
  }
  return out;
}

size_t pemSize(const std::string_view label, const size_t derBytes) {
  const size_t encoded = (derBytes + 2) / 3 * 4;
  const size_t lines = (encoded + 63) / 64;
  return (11 + label.size() + 6) + encoded + lines + (9 + label.size() + 6);
}

size_t pemWrite(const std::string_view label, const uint8_t* der, const size_t derBytes, char* out) {
  size_t n = 0;
  const auto put = [&](const std::string_view text) {
    for (const char c : text) out[n++] = c;
  };
  put("-----BEGIN ");
  put(label);
  put("-----\n");
  size_t column = 0;
  const auto emit = [&](const char c) {
    out[n++] = c;
    if (++column == 64) {
      out[n++] = '\n';
      column = 0;
    }
  };
  size_t i = 0;
  for (; i + 2 < derBytes; i += 3) {
    const uint32_t v = (uint32_t(der[i]) << 16) | (uint32_t(der[i + 1]) << 8) | der[i + 2];
    emit(ALPHABET[(v >> 18) & 63]);
    emit(ALPHABET[(v >> 12) & 63]);
    emit(ALPHABET[(v >> 6) & 63]);
    emit(ALPHABET[v & 63]);
  }
  if (i < derBytes) {
    uint32_t v = uint32_t(der[i]) << 16;
    if (i + 1 < derBytes) v |= uint32_t(der[i + 1]) << 8;
    emit(ALPHABET[(v >> 18) & 63]);
    emit(ALPHABET[(v >> 12) & 63]);
    emit(i + 1 < derBytes ? ALPHABET[(v >> 6) & 63] : '=');
    emit('=');
  }
  if (column != 0) out[n++] = '\n';
  put("-----END ");
  put(label);
  put("-----\n");
  return n;
}

Base64Decoder::~Base64Decoder() { reset(); }

void Base64Decoder::reset() {
  mbedtls_platform_zeroize(group_, sizeof(group_));
  have_ = 0;
  padded_ = false;
}

bool Base64Decoder::feed(const std::string_view text, uint8_t* out, const size_t capacity, size_t& written,
                         const bool allowSpace) {
  for (const char c : text) {
    if (allowSpace && space(c)) continue;
    if (padded_) return false;  // nothing may follow a padded group within a piece
    if (c == '=') {
      // Padding only in the third or fourth place of a group, and a third-place "=" must be followed by a fourth.
      if (have_ < 2) return false;
      group_[have_++] = 0xFF;
    } else {
      const int value = sextet(c);
      if (value < 0) return false;
      if (have_ > 0 && group_[have_ - 1] == 0xFF) return false;  // a character after "="
      group_[have_++] = static_cast<uint8_t>(value);
    }
    if (have_ < 4) continue;
    const bool pad3 = group_[2] == 0xFF;
    const bool pad4 = group_[3] == 0xFF;
    if (pad3 && !pad4) return false;
    const size_t bytes = pad3 ? 1 : pad4 ? 2 : 3;
    if (written + bytes > capacity) return false;
    const uint32_t v = (uint32_t(group_[0]) << 18) | (uint32_t(group_[1]) << 12) |
                       (uint32_t(pad3 ? 0 : group_[2]) << 6) | uint32_t(pad4 ? 0 : group_[3]);
    // Non-canonical encodings (bits left over under the padding) are refused: one text, one meaning.
    if ((pad3 && (v & 0xFFFF) != 0) || (pad4 && !pad3 && (v & 0xFF) != 0)) return false;
    out[written++] = static_cast<uint8_t>(v >> 16);
    if (bytes > 1) out[written++] = static_cast<uint8_t>(v >> 8);
    if (bytes > 2) out[written++] = static_cast<uint8_t>(v);
    padded_ = pad3 || pad4;
    mbedtls_platform_zeroize(group_, sizeof(group_));
    have_ = 0;
  }
  return true;
}

bool base64Decode(const std::string_view text, uint8_t* out, const size_t capacity, size_t& written,
                  const bool allowSpace) {
  Base64Decoder decoder;
  written = 0;
  return decoder.feed(text, out, capacity, written, allowSpace) && decoder.complete();
}

void toHex(const uint8_t* data, const size_t size, char* out) {
  constexpr char DIGITS[] = "0123456789abcdef";
  for (size_t i = 0; i < size; ++i) {
    out[2 * i] = DIGITS[data[i] >> 4];
    out[2 * i + 1] = DIGITS[data[i] & 15];
  }
  out[2 * size] = '\0';
}

bool fromHex64(const std::string_view text, uint8_t out[32]) {
  if (text.size() != 64) return false;
  for (size_t i = 0; i < 32; ++i) {
    const int hi = hexValue(text[2 * i]);
    const int lo = hexValue(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool sha256(const uint8_t* data, const size_t size, uint8_t out[32]) { return mbedtls_sha256(data, size, out, 0) == 0; }

}  // namespace netcodec
