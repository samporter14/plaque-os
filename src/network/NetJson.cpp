#include "NetJson.h"

#include <cstdio>
#include <limits>

namespace netjson {
namespace {

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t putUtf8(const uint32_t cp, uint8_t* out) {
  if (cp < 0x80) {
    if (out) out[0] = static_cast<uint8_t>(cp);
    return 1;
  }
  if (cp < 0x800) {
    if (out) {
      out[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
      out[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    }
    return 2;
  }
  if (cp < 0x10000) {
    if (out) {
      out[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
      out[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
      out[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
    }
    return 3;
  }
  if (out) {
    out[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
    out[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
    out[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
  }
  return 4;
}

// Length of one well-formed UTF-8 sequence at raw[i], or 0.
size_t utf8Sequence(const std::string_view raw, const size_t i) {
  const auto byte = [&](size_t k) { return static_cast<uint8_t>(raw[k]); };
  const uint8_t lead = byte(i);
  size_t length = 0;
  uint32_t cp = 0;
  uint32_t minimum = 0;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
    cp = lead & 0x1F;
    minimum = 0x80;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    cp = lead & 0x0F;
    minimum = 0x800;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    cp = lead & 0x07;
    minimum = 0x10000;
  } else {
    return 0;
  }
  if (i + length > raw.size()) return 0;
  for (size_t k = 1; k < length; ++k) {
    if ((byte(i + k) & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (byte(i + k) & 0x3F);
  }
  if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return length;
}

bool readHex4(const std::string_view raw, const size_t i, uint32_t& out) {
  if (i + 4 > raw.size()) return false;
  out = 0;
  for (size_t k = 0; k < 4; ++k) {
    const int v = hexValue(raw[i + k]);
    if (v < 0) return false;
    out = (out << 4) | static_cast<uint32_t>(v);
  }
  return true;
}

// Walks a string literal's raw contents (between the quotes). Validates it and returns its decoded length; writes
// the decoded bytes to `out` when given (sized for the decoded length). SIZE_MAX when the literal is malformed.
size_t walkString(const std::string_view raw, uint8_t* out) {
  size_t written = 0;
  for (size_t i = 0; i < raw.size();) {
    const auto c = static_cast<uint8_t>(raw[i]);
    if (c < 0x20) return SIZE_MAX;
    if (c >= 0x80) {
      const size_t length = utf8Sequence(raw, i);
      if (length == 0) return SIZE_MAX;
      for (size_t k = 0; k < length; ++k)
        if (out) out[written + k] = static_cast<uint8_t>(raw[i + k]);
      written += length;
      i += length;
      continue;
    }
    if (c != '\\') {
      if (out) out[written] = c;
      ++written;
      ++i;
      continue;
    }
    if (i + 1 >= raw.size()) return SIZE_MAX;
    const char e = raw[i + 1];
    uint8_t simple = 0;
    switch (e) {
      case '"':
        simple = '"';
        break;
      case '\\':
        simple = '\\';
        break;
      case '/':
        simple = '/';
        break;
      case 'b':
        simple = '\b';
        break;
      case 'f':
        simple = '\f';
        break;
      case 'n':
        simple = '\n';
        break;
      case 'r':
        simple = '\r';
        break;
      case 't':
        simple = '\t';
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!readHex4(raw, i + 2, cp)) return SIZE_MAX;
        i += 6;
        if (cp >= 0xDC00 && cp <= 0xDFFF) return SIZE_MAX;  // a low surrogate on its own
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          uint32_t low = 0;
          if (i + 1 >= raw.size() || raw[i] != '\\' || raw[i + 1] != 'u' || !readHex4(raw, i + 2, low) ||
              low < 0xDC00 || low > 0xDFFF)
            return SIZE_MAX;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        }
        written += putUtf8(cp, out ? out + written : nullptr);
        continue;
      }
      default:
        return SIZE_MAX;
    }
    if (out) out[written] = simple;
    ++written;
    i += 2;
  }
  return written;
}

class Parser {
  std::string_view s_;
  size_t pos_ = 0;
  std::vector<Node>& nodes_;
  size_t maxDepth_;
  size_t maxNodes_;

  void skipSpace() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
  }

  bool literal(const std::string_view word) {
    if (s_.substr(pos_, word.size()) != word) return false;
    pos_ += word.size();
    return true;
  }

  // The raw contents of a string literal starting at the opening quote.
  bool rawString(uint32_t& start, uint32_t& length, uint32_t& decoded) {
    if (pos_ >= s_.size() || s_[pos_] != '"') return false;
    const size_t begin = ++pos_;
    while (pos_ < s_.size() && s_[pos_] != '"') {
      if (s_[pos_] == '\\') ++pos_;  // the escaped character (validated by walkString)
      ++pos_;
    }
    if (pos_ >= s_.size()) return false;
    const std::string_view raw = s_.substr(begin, pos_ - begin);
    ++pos_;  // closing quote
    const size_t size = walkString(raw, nullptr);
    if (size == SIZE_MAX) return false;
    start = static_cast<uint32_t>(begin);
    length = static_cast<uint32_t>(raw.size());
    decoded = static_cast<uint32_t>(size);
    return true;
  }

  bool number(Node& node) {
    const size_t begin = pos_;
    if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
    if (pos_ >= s_.size()) return false;
    if (s_[pos_] == '0') {
      ++pos_;
    } else if (s_[pos_] >= '1' && s_[pos_] <= '9') {
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
    } else {
      return false;
    }
    if (pos_ < s_.size() && s_[pos_] == '.') {
      ++pos_;
      const size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == digits) return false;
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
      const size_t digits = pos_;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
      if (pos_ == digits) return false;
    }
    node.type = Type::Number;
    node.start = static_cast<uint32_t>(begin);
    node.length = static_cast<uint32_t>(pos_ - begin);
    return true;
  }

  int32_t add(Node&& node) {
    if (nodes_.size() >= maxNodes_) return -1;
    nodes_.push_back(std::move(node));
    return static_cast<int32_t>(nodes_.size() - 1);
  }

  // Links a parsed child under its parent (by index: the vector may have moved).
  static void link(std::vector<Node>& nodes, const int32_t parent, int32_t& last, const int32_t child) {
    if (last < 0) {
      nodes[parent].firstChild = child;
    } else {
      nodes[last].nextSibling = child;
    }
    last = child;
    ++nodes[parent].childCount;
  }

 public:
  Parser(std::string_view s, std::vector<Node>& nodes, size_t maxDepth, size_t maxNodes)
      : s_(s), nodes_(nodes), maxDepth_(maxDepth), maxNodes_(maxNodes) {}

  bool value(const size_t depth, int32_t& index, std::string key) {
    if (depth > maxDepth_) return false;
    skipSpace();
    if (pos_ >= s_.size()) return false;
    Node node;
    node.key = std::move(key);
    const char c = s_[pos_];
    if (c == '{' || c == '[') {
      const bool object = c == '{';
      node.type = object ? Type::Object : Type::Array;
      ++pos_;
      index = add(std::move(node));
      if (index < 0) return false;
      int32_t last = -1;
      skipSpace();
      if (pos_ < s_.size() && s_[pos_] == (object ? '}' : ']')) {
        ++pos_;
        return true;
      }
      while (true) {
        std::string name;
        if (object) {
          skipSpace();
          uint32_t start = 0, length = 0, decoded = 0;
          if (!rawString(start, length, decoded)) return false;
          name.resize(decoded);
          walkString(s_.substr(start, length), reinterpret_cast<uint8_t*>(name.data()));
          for (int32_t k = nodes_[index].firstChild; k >= 0; k = nodes_[k].nextSibling)
            if (nodes_[k].key == name) return false;  // a key given twice
          skipSpace();
          if (pos_ >= s_.size() || s_[pos_] != ':') return false;
          ++pos_;
        }
        int32_t child = -1;
        if (!value(depth + 1, child, std::move(name))) return false;
        link(nodes_, index, last, child);
        skipSpace();
        if (pos_ >= s_.size()) return false;
        if (s_[pos_] == ',') {
          ++pos_;
          continue;
        }
        if (s_[pos_] == (object ? '}' : ']')) {
          ++pos_;
          return true;
        }
        return false;
      }
    }
    if (c == '"') {
      node.type = Type::String;
      if (!rawString(node.start, node.length, node.decoded)) return false;
    } else if (c == 't' || c == 'f') {
      node.type = Type::Bool;
      node.boolean = c == 't';
      if (!literal(node.boolean ? "true" : "false")) return false;
    } else if (c == 'n') {
      node.type = Type::Null;
      if (!literal("null")) return false;
    } else if (!number(node)) {
      return false;
    }
    index = add(std::move(node));
    return index >= 0;
  }

  bool finished() {
    skipSpace();
    return pos_ == s_.size();
  }
};

}  // namespace

bool Document::parse(const std::string_view text, const size_t maxDepth, const size_t maxNodes) {
  text_ = text;
  nodes_.clear();
  nodes_.reserve(32);
  Parser parser(text, nodes_, maxDepth, maxNodes);
  int32_t root = -1;
  if (!parser.value(1, root, std::string()) || !parser.finished() || root != 0) {
    nodes_.clear();
    return false;
  }
  return true;
}

const Node* Document::first(const Node& node) const {
  return node.firstChild >= 0 ? &nodes_[static_cast<size_t>(node.firstChild)] : nullptr;
}

const Node* Document::next(const Node& node) const {
  return node.nextSibling >= 0 ? &nodes_[static_cast<size_t>(node.nextSibling)] : nullptr;
}

const Node* Document::child(const Node& node, size_t index) const {
  for (const Node* item = first(node); item; item = next(*item)) {
    if (index == 0) return item;
    --index;
  }
  return nullptr;
}

const Node* Document::member(const Node& object, const std::string_view key) const {
  if (object.type != Type::Object) return nullptr;
  for (const Node* item = first(object); item; item = next(*item))
    if (item->key == key) return item;
  return nullptr;
}

std::string Document::stringValue(const Node& node) const {
  if (node.type != Type::String) return {};
  std::string out(node.decoded, '\0');
  walkString(text_.substr(node.start, node.length), reinterpret_cast<uint8_t*>(out.data()));
  return out;
}

bool Document::secretValue(const Node& node, netprofile::SecretBytes& out) const {
  if (node.type != Type::String || !out.allocate(node.decoded)) return false;
  walkString(text_.substr(node.start, node.length), out.data());
  return true;
}

bool Document::integerValue(const Node& node, int64_t& out) const {
  if (node.type != Type::Number) return false;
  const std::string_view literal = text_.substr(node.start, node.length);
  size_t i = 0;
  const bool negative = !literal.empty() && literal[0] == '-';
  if (negative) i = 1;
  if (i >= literal.size()) return false;
  uint64_t magnitude = 0;
  for (; i < literal.size(); ++i) {
    const char c = literal[i];
    if (c < '0' || c > '9') return false;  // a fraction or an exponent
    const uint64_t digit = static_cast<uint64_t>(c - '0');
    if (magnitude > (static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) - digit) / 10) return false;
    magnitude = magnitude * 10 + digit;
  }
  out = negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude);
  return true;
}

void appendString(std::string& out, const std::string_view utf8) {
  out.push_back('"');
  for (size_t i = 0; i < utf8.size(); ++i) {
    const auto c = static_cast<uint8_t>(utf8[i]);
    if (c == '"' || c == '\\') {
      out.push_back('\\');
      out.push_back(static_cast<char>(c));
    } else if (c < 0x20 || c == 0x7F) {
      char escape[8];
      snprintf(escape, sizeof(escape), "\\u%04x", c);
      out += escape;
    } else if (c == 0xE2 && i + 2 < utf8.size() && static_cast<uint8_t>(utf8[i + 1]) == 0x80 &&
               (static_cast<uint8_t>(utf8[i + 2]) == 0xA8 || static_cast<uint8_t>(utf8[i + 2]) == 0xA9)) {
      out += static_cast<uint8_t>(utf8[i + 2]) == 0xA8 ? "\\u2028" : "\\u2029";
      i += 2;
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  out.push_back('"');
}

}  // namespace netjson
