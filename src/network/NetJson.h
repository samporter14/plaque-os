#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "NetSecret.h"

// A strict JSON reader for the network profile bundle. It refuses what a general parser would quietly accept:
// duplicate keys, malformed UTF-8, lone surrogates, raw control characters, trailing text, deep nesting. String
// values are not copied while parsing: a node records where its literal sits in the input, and the caller decodes it
// into a std::string (public fields) or a SecretBytes (a PSK, a private key) only when it needs it. The input stays
// the caller's, as does wiping it.
namespace netjson {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

struct Node {
  Type type = Type::Null;
  bool boolean = false;
  uint32_t start = 0;    // String: the first byte after the opening quote. Number: the literal's first byte.
  uint32_t length = 0;   // String: raw bytes up to the closing quote (escapes not decoded). Number: the literal.
  uint32_t decoded = 0;  // String: the decoded length in bytes
  int32_t firstChild = -1;
  int32_t nextSibling = -1;
  uint32_t childCount = 0;
  std::string key;  // a member of an object: its decoded name (names are never secret)
};

class Document {
  std::string_view text_;
  std::vector<Node> nodes_;

 public:
  // False when the text is not exactly one JSON value within the limits.
  bool parse(std::string_view text, size_t maxDepth = 6, size_t maxNodes = 128);
  const Node& root() const { return nodes_.front(); }
  const Node* child(const Node& node, size_t index) const;
  const Node* member(const Node& object, std::string_view key) const;
  // The members of an object, in order (or the items of an array).
  const Node* first(const Node& node) const;
  const Node* next(const Node& node) const;

  std::string stringValue(const Node& node) const;
  bool secretValue(const Node& node, netprofile::SecretBytes& out) const;
  // An integer written without fraction or exponent, within int64.
  bool integerValue(const Node& node, int64_t& out) const;
};

// Appends a JSON string literal (quotes included). Control characters and the two Unicode line separators are
// escaped; everything else is written as UTF-8.
void appendString(std::string& out, std::string_view utf8);

}  // namespace netjson
