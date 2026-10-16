#include "RegisterData.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <cstring>

#include "MethodTimers.h"

namespace reg {
namespace {
// Four bytes per character is the most UTF-8 spends; the character count itself is checked after decoding.
constexpr size_t MAX_TITLE_BYTES = 4 * MAX_TITLE_CHARS;

bool zoneChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '+' ||
         c == '-' || c == '/';
}

// A string member: present, a string, and no NUL inside it (ArduinoJson stops at the first one).
bool textMember(JsonVariantConst value, std::string& out, const size_t maxBytes) {
  if (!value.is<const char*>()) return false;
  const JsonString text = value.as<JsonString>();
  if (std::strlen(text.c_str()) != text.size() || text.size() > maxBytes) return false;
  out.assign(text.c_str(), text.size());
  return true;
}

// A whole number (not a float, a string or a boolean).
bool wholeNumber(JsonVariantConst value, int64_t& out) {
  if (!value.is<long long>()) return false;
  out = value.as<long long>();
  return true;
}

// The time of a reading: a whole number within MIN_EPOCH..MAX_EPOCH.
bool epochMember(JsonVariantConst value, int64_t& out) {
  int64_t seconds = 0;
  if (!wholeNumber(value, seconds) || seconds < static_cast<int64_t>(MIN_EPOCH) || seconds > static_cast<int64_t>(MAX_EPOCH))
    return false;
  out = seconds;
  return true;
}

enum class Entered : uint8_t { Kept, Dropped, Refused };

// One booking. Refused only when it is not an object (the reply is not what it says it is). An object with a bad title,
// a time that is not a whole number, an end that is not after its start or a flag that is not a boolean is Dropped, and
// so is one that lies wholly outside the years the reader can hold: one booking the reader cannot use does not cost the
// others. Kept (maybe clamped) otherwise.
Entered parseEntry(JsonVariantConst item, Entry& out) {
  if (!item.is<JsonObjectConst>()) return Entered::Refused;
  Entry entry;
  if (!textMember(item["title"], entry.title, MAX_TITLE_BYTES) || !validText(entry.title, MAX_TITLE_CHARS))
    return Entered::Dropped;
  int64_t start = 0;
  int64_t end = 0;
  if (!wholeNumber(item["start"], start) || !wholeNumber(item["end"], end) || end <= start) return Entered::Dropped;
  // "all_day" and "clamped" are present only when true, but a false is no harm; anything that is not a boolean is.
  for (const char* key : {"all_day", "clamped"}) {
    if (item[key].isUnbound()) continue;
    if (!item[key].is<bool>()) return Entered::Dropped;
    (key[0] == 'a' ? entry.allDay : entry.clamped) = item[key].as<bool>();
  }
  // Out of range is not malformed: one booking from 2019, or with no end in sight, must not cost the reply.
  const int64_t minEpoch = MIN_EPOCH;
  const int64_t maxEpoch = MAX_EPOCH;
  if (end <= minEpoch || start >= maxEpoch) return Entered::Dropped;
  if (start < minEpoch) start = minEpoch;
  if (end > maxEpoch || end - start > static_cast<int64_t>(MAX_BOOKING_SECONDS)) {
    end = std::min(maxEpoch, start + static_cast<int64_t>(MAX_BOOKING_SECONDS));
    entry.clamped = true;
  }
  entry.start = start;
  entry.end = end;
  out = std::move(entry);
  return Entered::Kept;
}

// The note: null or a string. Cut to MAX_NOTE_CHARS characters, and dropped (not refused) when it is not printable text.
bool parseNote(JsonVariantConst member, std::string& out) {
  out.clear();
  if (member.isUnbound()) return false;  // the key must be there
  if (member.isNull()) return true;
  if (!member.is<const char*>()) return false;
  const JsonString text = member.as<JsonString>();
  if (std::strlen(text.c_str()) != text.size()) return true;  // a NUL inside: not text
  size_t keep = text.size();
  size_t characters = 0;
  for (size_t i = 0; i < text.size();) {
    if (characters == MAX_NOTE_CHARS) {
      keep = i;
      break;
    }
    const unsigned char lead = static_cast<unsigned char>(text.c_str()[i]);
    i += lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    ++characters;
  }
  keep = std::min(keep, text.size());
  if (keep == 0 || method::printableCharCount(text.c_str(), keep) < 1) return true;  // empty or not printable: no note
  out.assign(text.c_str(), keep);
  return true;
}

// null, or one booking.
bool parseOptionalEntry(JsonVariantConst member, bool& present, Entry& out, int& dropped) {
  if (member.isUnbound()) return false;  // the key must be there
  present = false;
  if (member.isNull()) return true;
  const Entered result = parseEntry(member, out);
  present = result == Entered::Kept;
  if (result == Entered::Dropped) ++dropped;
  return result != Entered::Refused;
}

bool parseDay(JsonVariantConst member, std::vector<Entry>& out, int& dropped) {
  if (!member.is<JsonArrayConst>()) return false;
  const JsonArrayConst array = member.as<JsonArrayConst>();
  if (array.size() > MAX_DAY_ENTRIES) return false;
  out.reserve(array.size());
  for (JsonVariantConst item : array) {
    Entry entry;
    const Entered result = parseEntry(item, entry);
    if (result == Entered::Refused) return false;
    if (result == Entered::Kept) out.push_back(std::move(entry));
    if (result == Entered::Dropped) ++dropped;
  }
  std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    return a.start != b.start ? a.start < b.start : a.end < b.end;
  });
  return true;
}

// The optional zone. False (and nothing kept) when it is absent or fails any check: the reply is still good.
bool parseZone(JsonVariantConst member, Zone& out) {
  out = Zone();
  if (member.isUnbound() || !member.is<JsonObjectConst>()) return false;
  Zone zone;
  if (!textMember(member["name"], zone.name, MAX_ZONE_NAME_BYTES) ||
      method::printableCharCount(zone.name.c_str(), zone.name.size()) < 1)
    return false;
  if (!member["spans"].is<JsonArrayConst>()) return false;
  const JsonArrayConst spans = member["spans"].as<JsonArrayConst>();
  if (spans.size() < 1 || spans.size() > MAX_ZONE_SPANS) return false;
  for (JsonVariantConst item : spans) {
    int64_t from = 0;
    int64_t offset = 0;
    if (!item.is<JsonObjectConst>() || !wholeNumber(item["from"], from) || !wholeNumber(item["offset"], offset)) return false;
    if (from < 0 || from > static_cast<int64_t>(MAX_EPOCH) || offset < -MAX_ZONE_OFFSET_S || offset > MAX_ZONE_OFFSET_S)
      return false;
    if (!zone.spans.empty() && from <= zone.spans.back().from) return false;  // strictly ascending
    zone.spans.push_back({from, static_cast<int32_t>(offset)});
  }
  out = std::move(zone);
  return true;
}

void putEntry(JsonObject object, const Entry& entry) {
  object["title"] = entry.title;
  object["start"] = static_cast<uint32_t>(entry.start);
  object["end"] = static_cast<uint32_t>(entry.end);
  if (entry.allDay) object["all_day"] = true;
  if (entry.clamped) object["clamped"] = true;
}

void putDay(JsonArray array, const std::vector<Entry>& entries) {
  for (const Entry& entry : entries) putEntry(array.add<JsonObject>(), entry);
}

const char* statusName(const Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::Stale:
      return "stale";
    case Status::Unavailable:
      break;
  }
  return "unavailable";
}
}  // namespace

namespace {
// Text that shows nothing: only spaces. printableCharCount() accepts a space (it is printable), but a title or an
// instrument name made of them would draw as a blank. The spaces a person can type or paste: U+0020, the no-break space,
// the en to hair spaces and the ideographic space.
bool onlySpaces(const std::string_view text) {
  for (size_t i = 0; i < text.size();) {
    const auto at = [&](const size_t offset) { return i + offset < text.size() ? static_cast<unsigned char>(text[i + offset]) : 0; };
    if (at(0) == 0x20) {
      i += 1;
    } else if (at(0) == 0xC2 && at(1) == 0xA0) {
      i += 2;
    } else if (at(0) == 0xE2 && at(1) == 0x80 && at(2) >= 0x80 && at(2) <= 0x8A) {
      i += 3;
    } else if (at(0) == 0xE3 && at(1) == 0x80 && at(2) == 0x80) {
      i += 3;
    } else {
      return false;
    }
  }
  return true;
}
}  // namespace

bool validText(const std::string_view text, const size_t maxChars) {
  if (text.empty() || text.size() > 4 * maxChars) return false;
  const int characters = method::printableCharCount(text.data(), text.size());
  return characters >= 1 && static_cast<size_t>(characters) <= maxChars && !onlySpaces(text);
}

bool parse(const std::string_view json, Schedule& out, const size_t maxBytes) {
  if (json.empty() || json.size() > maxBytes) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(NESTING_LIMIT)) ||
      !doc.is<JsonObject>())
    return false;

  Schedule s;
  if (!textMember(doc["instrument"], s.instrument, MAX_TITLE_BYTES) || !validText(s.instrument, MAX_TITLE_CHARS))
    return false;
  if (!textMember(doc["timezone"], s.timezone, MAX_ZONE_CHARS) || s.timezone.empty() ||
      !std::all_of(s.timezone.begin(), s.timezone.end(), zoneChar))
    return false;

  std::string status;
  if (!textMember(doc["status"], status, 16)) return false;
  if (status == "ok") {
    s.status = Status::Ok;
  } else if (status == "stale") {
    s.status = Status::Stale;
  } else if (status == "unavailable") {
    s.status = Status::Unavailable;
  } else {
    return false;
  }

  const JsonVariantConst fetched = doc["fetched_at"];
  if (fetched.isUnbound()) return false;
  if (s.status == Status::Unavailable) {
    if (!fetched.isNull()) return false;  // never read: there is no time to state
  } else if (!epochMember(fetched, s.fetchedAt)) {
    return false;
  }

  if (!parseNote(doc["note"], s.note)) return false;
  s.hasZone = parseZone(doc["zone"], s.zone);

  int dropped = 0;
  if (!parseOptionalEntry(doc["now"], s.hasNow, s.now, dropped) || !parseOptionalEntry(doc["next"], s.hasNext, s.next, dropped) ||
      !parseDay(doc["today"], s.today, dropped) || !parseDay(doc["tomorrow"], s.tomorrow, dropped))
    return false;
  // A booking that was dropped is one nobody will see: the note says so (first, so the cut to MAX_NOTE_CHARS keeps it),
  // and the screen will not promise "Free". A booking named twice (now, and in today) counts twice; it is a count of
  // what could not be read, not of bookings.
  if (dropped > 0) {
    std::string sentence = dropped == 1 ? "1 booking couldn't be read." : std::to_string(dropped) + " bookings couldn't be read.";
    s.note = s.note.empty() ? sentence : sentence + " " + s.note;
    // Cut to the note limit between characters.
    size_t chars = 0;
    size_t keep = s.note.size();
    for (size_t i = 0; i < s.note.size();) {
      if (chars == MAX_NOTE_CHARS) {
        keep = i;
        break;
      }
      const unsigned char lead = static_cast<unsigned char>(s.note[i]);
      i += lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
      ++chars;
    }
    s.note.resize(std::min(keep, s.note.size()));
  }
  if (s.status == Status::Unavailable && (s.hasNow || s.hasNext || !s.today.empty() || !s.tomorrow.empty()))
    return false;

  out = std::move(s);
  return true;
}

std::string toJson(const Schedule& schedule) {
  JsonDocument doc;
  doc["instrument"] = schedule.instrument;
  doc["timezone"] = schedule.timezone;
  if (schedule.status == Status::Unavailable) {
    doc["fetched_at"] = nullptr;
  } else {
    doc["fetched_at"] = static_cast<uint32_t>(schedule.fetchedAt);
  }
  doc["status"] = statusName(schedule.status);
  if (schedule.note.empty()) {
    doc["note"] = nullptr;
  } else {
    doc["note"] = schedule.note;
  }
  if (schedule.hasNow) {
    putEntry(doc["now"].to<JsonObject>(), schedule.now);
  } else {
    doc["now"] = nullptr;
  }
  if (schedule.hasNext) {
    putEntry(doc["next"].to<JsonObject>(), schedule.next);
  } else {
    doc["next"] = nullptr;
  }
  if (schedule.hasZone) {
    JsonObject zone = doc["zone"].to<JsonObject>();
    zone["name"] = schedule.zone.name;
    JsonArray spans = zone["spans"].to<JsonArray>();
    for (const ZoneSpan& span : schedule.zone.spans) {
      JsonObject item = spans.add<JsonObject>();
      item["from"] = static_cast<uint32_t>(span.from);
      item["offset"] = span.offset;
    }
  }
  putDay(doc["today"].to<JsonArray>(), schedule.today);
  putDay(doc["tomorrow"].to<JsonArray>(), schedule.tomorrow);
  std::string out;
  serializeJson(doc, out);
  return out;
}

CardLoad loadCard(CardFiles& card, Schedule& out) {
  std::string text;
  Schedule parsed;
  if (card.exists(CARD_PATH)) {
    if (!card.read(CARD_PATH, MAX_CARD_BYTES, text) || !parse(text, parsed, MAX_CARD_BYTES)) return CardLoad::None;
    out = std::move(parsed);
    return CardLoad::Main;
  }
  if (!card.exists(CARD_TMP_PATH)) return CardLoad::None;
  // A save removes the old file before it renames the new one: a power cut between the two leaves the complete temporary
  // file and no register.json. It is only trusted as far as the parser trusts it.
  if (!card.read(CARD_TMP_PATH, MAX_CARD_BYTES, text) || !parse(text, parsed, MAX_CARD_BYTES)) {
    card.remove(CARD_TMP_PATH);
    return CardLoad::None;
  }
  if (!card.rename(CARD_TMP_PATH, CARD_PATH)) card.remove(CARD_TMP_PATH);  // the schedule is used this boot all the same
  out = std::move(parsed);
  return CardLoad::Recovered;
}

}  // namespace reg
