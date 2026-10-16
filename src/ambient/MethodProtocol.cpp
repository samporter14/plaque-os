#include "MethodProtocol.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MessageProtocol.h"
#include "MethodTimers.h"

namespace method {
namespace {
constexpr size_t MAX_NAME_CHARS = 40;
constexpr char FALLBACK_NAME[] = "Book";
constexpr char EXTENSION[] = ".xtc";

bool continuationByte(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Characters in [1, MAX_TITLE_CHARS] and no bytes beyond MAX_TITLE_BYTES.
bool validTitle(std::string_view title) {
  if (title.empty() || title.size() > MAX_TITLE_BYTES) return false;
  size_t chars = 0;
  for (const char c : title)
    if (!continuationByte(c)) ++chars;
  return chars >= 1 && chars <= MAX_TITLE_CHARS;
}

// Reads a string member without trusting it to be NUL-free.
bool stringMember(JsonVariantConst value, std::string_view& out) {
  if (!value.is<const char*>()) return false;
  const auto text = value.as<JsonString>();
  out = std::string_view(text.c_str(), text.size());
  return std::strlen(text.c_str()) == text.size();
}

template <size_t N>
void copyInto(char (&dst)[N], std::string_view src) {
  memcpy(dst, src.data(), src.size());
  dst[src.size()] = '\0';
}
}  // namespace

const Timer* timerOnPage(const std::vector<Timer>& timers, unsigned page) {
  const auto it = std::find_if(timers.begin(), timers.end(), [page](const Timer& t) { return t.page == page; });
  return it == timers.end() ? nullptr : &*it;
}

bool validBookId(std::string_view id) {
  // The Worker issues "b<version>"; keep to that shape so the id is safe in a URL path.
  if (id.size() < 2 || id.size() > 20 || id.front() != 'b') return false;
  return std::all_of(id.begin() + 1, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool routeAllowed(std::string_view deviceId, std::string_view path) {
  if (!ambient::validIdentifier(deviceId, 32)) return false;
  const std::string_view head("/api/device/");
  if (path.substr(0, head.size()) != head) return false;
  path.remove_prefix(head.size());
  if (path.substr(0, deviceId.size()) != deviceId) return false;
  path.remove_prefix(deviceId.size());
  if (path == "/books") return true;
  const std::string_view book("/book/");
  return path.substr(0, book.size()) == book && validBookId(path.substr(book.size()));
}

std::string urlHost(std::string_view url) {
  const std::string_view https("https://");
  const std::string_view http("http://");
  if (url.substr(0, https.size()) == https) {
    url.remove_prefix(https.size());
  } else if (url.substr(0, http.size()) == http) {
    url.remove_prefix(http.size());
  } else {
    return {};
  }
  url = url.substr(0, url.find_first_of("/?#"));
  const size_t at = url.rfind('@');
  if (at != std::string_view::npos) url.remove_prefix(at + 1);
  if (!url.empty() && url.front() == '[') return {};  // an IPv6 literal
  url = url.substr(0, url.find(':'));
  return std::string(url);
}

bool isIpv4Literal(std::string_view host) {
  int parts = 0;
  while (true) {
    const size_t dot = host.find('.');
    const std::string_view part = host.substr(0, dot);
    if (part.empty() || part.size() > 3 ||
        !std::all_of(part.begin(), part.end(), [](char c) { return c >= '0' && c <= '9'; }))
      return false;
    int value = 0;
    for (const char c : part) value = value * 10 + (c - '0');
    if (value > 255) return false;
    ++parts;
    if (dot == std::string_view::npos) break;
    host.remove_prefix(dot + 1);
  }
  return parts == 4;
}

bool validSha256Hex(std::string_view value) {
  return value.size() == SHA256_HEX_BYTES && std::all_of(value.begin(), value.end(), [](char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

void toHex(const uint8_t digest[32], char out[SHA256_HEX_BYTES + 1]) {
  for (size_t i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

size_t utf8Prefix(std::string_view text, size_t maxBytes) {
  if (text.size() <= maxBytes) return text.size();
  size_t keep = maxBytes;
  while (keep > 0 && continuationByte(text[keep])) --keep;
  return keep;
}

namespace {
// One book description, as /book returns it and as each /books entry carries it. Strict: the book_url
// must be exactly this device's route for this book id, so the Bearer token can only ever be sent there.
bool parseEntry(JsonVariantConst item, std::string_view deviceId, BookMetadata& result) {
  if (!item.is<JsonObjectConst>()) return false;
  std::string_view id, title, hash, url;
  if (!stringMember(item["book_id"], id) || !stringMember(item["title"], title) ||
      !stringMember(item["sha256"], hash) || !stringMember(item["book_url"], url) ||
      !item["byte_length"].is<unsigned>() || !item["page_count"].is<unsigned>())
    return false;
  const unsigned byteLength = item["byte_length"].as<unsigned>();
  const unsigned pageCount = item["page_count"].as<unsigned>();
  uint32_t idNumber = 0;
  if (!validBookId(id) || !bookIdNumber(id, idNumber) || !validTitle(title) || !validSha256Hex(hash) ||
      byteLength < 1 || byteLength > MAX_BOOK_BYTES || pageCount < 1 || pageCount > MAX_PAGES)
    return false;
  // /book has no kind (a method); a /books entry names one, and an unknown name is refused.
  Kind kind = Kind::Method;
  if (!item["kind"].isUnbound()) {
    std::string_view name;
    if (!stringMember(item["kind"], name) || !parseKind(name, kind)) return false;
  }
  char expected[96];
  const int written =
      snprintf(expected, sizeof(expected), "/api/device/%.*s/book/%.*s", static_cast<int>(deviceId.size()),
               deviceId.data(), static_cast<int>(id.size()), id.data());
  if (written < 0 || static_cast<size_t>(written) >= sizeof(expected) || url != expected) return false;
  BookMetadata parsed;
  copyInto(parsed.bookId, id);
  copyInto(parsed.title, title);
  // The title is only displayed and reduced to a safe file name, so a stray tab or newline
  // becomes a space rather than making the whole book unreadable.
  for (char& c : parsed.title)
    if (static_cast<unsigned char>(c) < 0x20 && c != '\0') c = ' ';
  copyInto(parsed.sha256, hash);
  copyInto(parsed.bookUrl, url);
  parsed.byteLength = byteLength;
  parsed.pageCount = static_cast<uint16_t>(pageCount);
  parsed.kind = kind;
  parsed.idNumber = idNumber;
  parsed.empty = false;
  // The timers belong to the book: all of them or none. A set that does not validate is dropped on its own; the
  // Worker refuses to store one, so seeing one means damage, and the book is still worth keeping.
  if (!item["timers"].isUnbound() && !parseTimerArray(item["timers"], pageCount, parsed.timers)) {
    parsed.timers.clear();
    parsed.timersDropped = true;
  }
  result = parsed;
  return true;
}
}  // namespace

bool bookIdNumber(std::string_view id, uint32_t& out) {
  if (!validBookId(id) || id.size() > 11) return false;  // "b" and up to 10 digits
  uint64_t value = 0;
  for (const char c : id.substr(1)) value = value * 10 + static_cast<uint64_t>(c - '0');
  if (value > UINT32_MAX) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

const char* kindName(const Kind kind) {
  switch (kind) {
    case Kind::Method:
      return "method";
    case Kind::Table:
      return "table";
    case Kind::Plate:
      return "plate";
    case Kind::Board:
      return "board";
  }
  return "method";
}

bool parseKind(std::string_view name, Kind& out) {
  for (const Kind kind : {Kind::Method, Kind::Table, Kind::Plate, Kind::Board}) {
    if (name == kindName(kind)) {
      out = kind;
      return true;
    }
  }
  return false;
}

bool parseBook(std::string_view json, std::string_view deviceId, BookMetadata& result) {
  if (json.empty() || json.size() > MAX_METADATA_BYTES || !ambient::validIdentifier(deviceId, 32)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(5)) || !doc.is<JsonObject>() ||
      doc["book_id"].isUnbound())
    return false;
  if (doc["book_id"].isNull()) {
    result = {};
    return true;
  }
  return parseEntry(doc.as<JsonVariantConst>(), deviceId, result);
}

bool parseBooks(std::string_view json, std::string_view deviceId, std::vector<BookMetadata>& out, uint32_t& newestId,
                size_t& skipped) {
  out.clear();
  newestId = 0;
  skipped = 0;
  if (json.empty() || json.size() > MAX_LIST_BYTES || !ambient::validIdentifier(deviceId, 32)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(LIST_NESTING_LIMIT)) ||
      !doc.is<JsonObject>() || !doc["books"].is<JsonArrayConst>())
    return false;
  out.reserve(MAX_LIST_BOOKS);
  size_t seen = 0;
  for (JsonVariantConst item : doc["books"].as<JsonArrayConst>()) {
    if (++seen > MAX_LIST_BOOKS) break;  // the Worker sends at most twelve
    BookMetadata book;
    // A damaged entry is skipped on its own: one bad row must not hide the good ones, and a damaged row
    // never reaches the Bearer-token request because its book_url is never used.
    const bool duplicate = std::any_of(out.begin(), out.end(), [&](const BookMetadata& b) {
      return std::strcmp(b.bookId, item["book_id"] | "") == 0;
    });
    if (duplicate || !parseEntry(item, deviceId, book)) {
      ++skipped;
      continue;
    }
    newestId = std::max(newestId, book.idNumber);
    out.push_back(book);
  }
  return true;
}

bool bookFileName(std::string_view title, std::string_view sha256, char* out, size_t outSize) {
  if (!out || !validSha256Hex(sha256)) return false;
  char name[MAX_NAME_CHARS + 1];
  size_t length = 0;
  bool pendingSpace = false;
  for (const char c : title) {
    const bool keep =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (keep) {
      if (pendingSpace && length > 0 && length < MAX_NAME_CHARS) name[length++] = ' ';
      pendingSpace = false;
      if (length < MAX_NAME_CHARS) name[length++] = c;
    } else {
      // Spaces, other punctuation and every non-ASCII byte collapse to one space.
      pendingSpace = true;
    }
  }
  while (length > 0 && name[length - 1] == ' ') --length;
  if (length == 0) {
    memcpy(name, FALLBACK_NAME, sizeof(FALLBACK_NAME));
    length = sizeof(FALLBACK_NAME) - 1;
  }
  name[length] = '\0';
  const int written =
      snprintf(out, outSize, "%s %.*s%s", name, static_cast<int>(FILE_SHA_CHARS), sha256.data(), EXTENSION);
  return written > 0 && static_cast<size_t>(written) < outSize;
}

bool bookPath(std::string_view title, std::string_view sha256, char* out, size_t outSize) {
  const size_t prefix = sizeof(DIRECTORY_PREFIX) - 1;
  if (!out || outSize <= prefix) return false;
  memcpy(out, DIRECTORY_PREFIX, prefix);
  return bookFileName(title, sha256, out + prefix, outSize - prefix);
}

bool ownedBookPath(std::string_view path) {
  const std::string_view prefix(DIRECTORY_PREFIX);
  const std::string_view extension(EXTENSION);
  if (path.size() <= prefix.size() + extension.size() || path.size() >= MAX_PATH_BYTES ||
      path.substr(0, prefix.size()) != prefix)
    return false;
  const auto name = path.substr(prefix.size());
  if (name.front() == '.' || name.find('/') != std::string_view::npos || name.find("..") != std::string_view::npos ||
      name.substr(name.size() - extension.size()) != extension)
    return false;
  return std::all_of(name.begin(), name.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           c == ' ' || c == '.';
  });
}
}  // namespace method
