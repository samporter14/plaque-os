#include "MethodTimers.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace method {
namespace {
// One UTF-8 character at text[i]: its length (1 to 4) and code point, or 0 when the bytes are not well-formed UTF-8
// (a stray continuation byte, an overlong form, a surrogate, a code point past U+10FFFF, a cut-off sequence).
size_t decodeUtf8(const char* text, size_t size, size_t i, uint32_t& cp) {
  const auto byte = [&](size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(text[k])); };
  const uint32_t lead = byte(i);
  size_t length = 0;
  uint32_t minimum = 0;
  if (lead < 0x80) {
    cp = lead;
    return 1;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
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
  if (i + length > size) return 0;
  for (size_t k = 1; k < length; ++k) {
    if ((byte(i + k) & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (byte(i + k) & 0x3F);
  }
  if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return length;
}

// Text a person typed that ends up on the reader's screen, as the Worker has it: no control characters (C0, DEL, C1),
// no invisible format characters (soft hyphen, U+200B..U+200F, U+202A..U+202E, U+2060..U+206F, the byte order mark
// ...), no line or paragraph separators. Letters, marks, symbols and emoji are fine.
bool unprintable(uint32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) return true;
  switch (cp) {
    case 0x00AD:
    case 0x061C:
    case 0x06DD:
    case 0x070F:
    case 0x08E2:
    case 0x180E:
    case 0x2028:
    case 0x2029:
    case 0xFEFF:
    case 0x110BD:
    case 0x110CD:
    case 0xE0001:
      return true;
    default:
      break;
  }
  return (cp >= 0x0600 && cp <= 0x0605) || (cp >= 0x0890 && cp <= 0x0891) || (cp >= 0x200B && cp <= 0x200F) ||
         (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) ||
         (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0x13430 && cp <= 0x1343F) || (cp >= 0x1BCA0 && cp <= 0x1BCA3) ||
         (cp >= 0x1D173 && cp <= 0x1D17A) || (cp >= 0xE0020 && cp <= 0xE007F);
}

bool wholeNumber(JsonVariantConst value, uint32_t& out) {
  if (!value.is<uint32_t>()) return false;
  out = value.as<uint32_t>();
  return true;
}

bool knownKey(const char* key) {
  return std::strcmp(key, "page") == 0 || std::strcmp(key, "seconds") == 0 || std::strcmp(key, "label") == 0 ||
         std::strcmp(key, "box") == 0;
}

bool parseTimer(JsonVariantConst item, unsigned pageCount, Timer& timer) {
  if (!item.is<JsonObjectConst>()) return false;
  for (JsonPairConst member : item.as<JsonObjectConst>())
    if (!knownKey(member.key().c_str())) return false;

  uint32_t page = 0, seconds = 0;
  if (!wholeNumber(item["page"], page) || page < 1 || page > pageCount || page > UINT16_MAX) return false;
  if (!wholeNumber(item["seconds"], seconds) || seconds < 1 || seconds > MAX_TIMER_SECONDS) return false;

  std::string label;
  if (!item["label"].isUnbound()) {
    if (!item["label"].is<const char*>()) return false;
    const auto text = item["label"].as<JsonString>();
    if (std::strlen(text.c_str()) != text.size()) return false;  // a NUL inside the text
    if (text.size() > MAX_TIMER_LABEL_BYTES) return false;
    size_t characters = 0;
    for (size_t i = 0; i < text.size();) {
      uint32_t cp = 0;
      const size_t length = decodeUtf8(text.c_str(), text.size(), i, cp);
      if (length == 0 || unprintable(cp)) return false;  // malformed text, or text the Worker would have refused
      i += length;
      ++characters;
    }
    if (characters < 1 || characters > MAX_TIMER_LABEL_CHARS) return false;
    label.assign(text.c_str(), text.size());
  }

  const JsonVariantConst box = item["box"];
  if (!box.is<JsonArrayConst>() || box.size() != 4) return false;
  uint32_t b[4] = {};
  for (size_t i = 0; i < 4; ++i)
    if (!wholeNumber(box[i], b[i])) return false;
  const uint32_t x = b[0], y = b[1], w = b[2], h = b[3];
  if (w < static_cast<uint32_t>(MIN_TIMER_BOX_PX) || h < static_cast<uint32_t>(MIN_TIMER_BOX_PX)) return false;
  // Sums in 64 bits: two numbers near 2^32 must not wrap round to something small enough to pass.
  if (static_cast<uint64_t>(x) + w > static_cast<uint64_t>(PAGE_WIDTH_PX) ||
      static_cast<uint64_t>(y) + h > static_cast<uint64_t>(PAGE_HEIGHT_PX))
    return false;

  timer.page = static_cast<uint16_t>(page);
  timer.seconds = seconds;
  timer.label = std::move(label);
  timer.x = static_cast<uint16_t>(x);
  timer.y = static_cast<uint16_t>(y);
  timer.w = static_cast<uint16_t>(w);
  timer.h = static_cast<uint16_t>(h);
  return true;
}

void appendString(std::string& out, const std::string& text) {
  out += '"';
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (byte < 0x20) {
      char escape[8];
      snprintf(escape, sizeof(escape), "\\u%04x", byte);
      out += escape;
    } else {
      out += c;
    }
  }
  out += '"';
}
}  // namespace

int printableCharCount(const char* text, const size_t size) {
  int characters = 0;
  for (size_t i = 0; i < size;) {
    uint32_t cp = 0;
    const size_t length = decodeUtf8(text, size, i, cp);
    if (length == 0 || unprintable(cp)) return -1;
    i += length;
    ++characters;
  }
  return characters;
}

bool parseTimerArray(JsonVariantConst value, unsigned pageCount, std::vector<Timer>& out) {
  out.clear();
  if (!value.is<JsonArrayConst>()) return false;
  const JsonArrayConst array = value.as<JsonArrayConst>();
  if (array.size() > MAX_TIMERS || measureJson(value) > MAX_TIMERS_JSON_BYTES) return false;
  std::vector<Timer> parsed;
  parsed.reserve(array.size());
  for (JsonVariantConst item : array) {
    Timer timer;
    if (!parseTimer(item, pageCount, timer)) return false;
    // At most one timer per page.
    if (std::any_of(parsed.begin(), parsed.end(), [&](const Timer& t) { return t.page == timer.page; })) return false;
    parsed.push_back(std::move(timer));
  }
  out = std::move(parsed);
  return true;
}

void appendTimersJson(std::string& out, const std::vector<Timer>& timers) {
  out += '[';
  for (size_t i = 0; i < timers.size(); ++i) {
    const Timer& t = timers[i];
    char head[48];
    snprintf(head, sizeof(head), "%s{\"page\":%u,\"seconds\":%lu", i ? "," : "", static_cast<unsigned>(t.page),
             static_cast<unsigned long>(t.seconds));
    out += head;
    if (!t.label.empty()) {
      out += ",\"label\":";
      appendString(out, t.label);
    }
    char box[64];
    snprintf(box, sizeof(box), ",\"box\":[%u,%u,%u,%u]}", static_cast<unsigned>(t.x), static_cast<unsigned>(t.y),
             static_cast<unsigned>(t.w), static_cast<unsigned>(t.h));
    out += box;
  }
  out += ']';
}

}  // namespace method
