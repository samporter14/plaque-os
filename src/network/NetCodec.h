#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Encodings the network profile code shares: base64 (the USB protocol and PEM), lowercase hex and SHA-256.
namespace netcodec {

// Standard alphabet, padded. For data that is not secret (the INFO, PROFILE and RESULT replies).
std::string base64Encode(const uint8_t* data, size_t size);

// A strict base64 decoder that can be fed in pieces. Only the standard alphabet; a group may end in "=" or "==",
// after which nothing may follow until endPiece() (a USB DATA line may be encoded on its own) or the end. Decoded
// bytes are written straight to the caller's buffer; the four characters held between pieces are zeroed when the
// decoder is reset or destroyed, because they may be part of a secret.
class Base64Decoder {
  uint8_t group_[4] = {};
  size_t have_ = 0;
  bool padded_ = false;  // the last group ended in padding: nothing more in this piece

 public:
  ~Base64Decoder();
  void reset();
  // Feeds characters. White space is skipped when allowSpace (PEM bodies), refused otherwise. False on a character
  // outside the alphabet, misplaced padding, or output that would not fit in `capacity`.
  bool feed(std::string_view text, uint8_t* out, size_t capacity, size_t& written, bool allowSpace);
  // The end of a piece: padding may have ended it, and the next piece starts a new group.
  void endPiece() { padded_ = false; }
  // True when no partial group is waiting (the input so far was whole groups).
  bool complete() const { return have_ == 0; }
};

// Decodes a whole base64 text (white space allowed). The output is at most text.size() * 3 / 4 bytes.
bool base64Decode(std::string_view text, uint8_t* out, size_t capacity, size_t& written, bool allowSpace);

// PEM of one DER block: "-----BEGIN <label>-----", the base64 in lines of 64, "-----END <label>-----", each line
// ending in "\n". Written straight into the caller's buffer (no temporary copy, so a key can go into a SecretBytes).
size_t pemSize(std::string_view label, size_t derBytes);
size_t pemWrite(std::string_view label, const uint8_t* der, size_t derBytes, char* out);

// Lowercase hex; `out` holds 2 * size + 1 characters.
void toHex(const uint8_t* data, size_t size, char* out);
// Exactly 64 hex digits, either case, into 32 bytes.
bool fromHex64(std::string_view text, uint8_t out[32]);

bool sha256(const uint8_t* data, size_t size, uint8_t out[32]);

}  // namespace netcodec
