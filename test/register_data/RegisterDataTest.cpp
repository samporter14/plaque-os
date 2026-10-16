#include <cstdlib>
#include <iostream>
#include <map>
#include <string>

#include "RegisterData.h"

namespace {
int checks = 0;
void check(bool condition, const std::string& name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

// A reply the way the Worker sends it (docs/SPEC-REGISTER.md section 1), built from pieces so a test can break one.
constexpr long long FETCHED = 1792000000;

std::string entry(const std::string& title, long long start, long long end) {
  return "{\"title\":\"" + title + "\",\"start\":" + std::to_string(start) + ",\"end\":" + std::to_string(end) + "}";
}

struct Reply {
  std::string instrument = "\"Centrifuge 5424\"";
  std::string timezone = "\"America/Detroit\"";
  std::string fetched = std::to_string(FETCHED);
  std::string status = "\"ok\"";
  std::string note = "null";
  std::string now = entry("Ana", FETCHED - 600, FETCHED + 3000);
  std::string next = entry("Sam", FETCHED + 10000, FETCHED + 17200);
  std::string today = "[" + entry("Ana", FETCHED - 600, FETCHED + 3000) + "," + entry("Sam", FETCHED + 10000, FETCHED + 17200) + "]";
  std::string tomorrow = "[]";
  std::string extra;  // more members, ahead of the rest

  std::string text() const {
    return "{" + extra + "\"instrument\":" + instrument + ",\"timezone\":" + timezone + ",\"fetched_at\":" + fetched +
           ",\"status\":" + status + ",\"note\":" + note + ",\"now\":" + now + ",\"next\":" + next +
           ",\"today\":" + today + ",\"tomorrow\":" + tomorrow + "}";
  }
};

bool accepts(const Reply& reply) {
  reg::Schedule s;
  return reg::parse(reply.text(), s);
}

// The reply is kept, one booking in it could not be used, and the note says so.
bool dropsOne(const Reply& reply) {
  reg::Schedule s;
  return reg::parse(reply.text(), s) && s.note.rfind("1 booking couldn't be read.", 0) == 0;
}

std::string repeat(const std::string& unit, int times) {
  std::string out;
  for (int i = 0; i < times; ++i) out += unit;
  return out;
}

std::string dayOf(int count) {
  std::string out = "[";
  for (int i = 0; i < count; ++i) {
    if (i) out += ",";
    out += entry("B" + std::to_string(i), FETCHED + i * 4000, FETCHED + i * 4000 + 3000);
  }
  return out + "]";
}
}  // namespace

int main() {
  // ---- the reply in the spec -------------------------------------------------------------------------------------
  {
    reg::Schedule s;
    check(reg::parse(Reply().text(), s), "the spec's example reply parses");
    check(s.instrument == "Centrifuge 5424" && s.timezone == "America/Detroit", "names");
    check(s.fetchedAt == FETCHED && s.status == reg::Status::Ok && s.note.empty(), "time, status, no note");
    check(s.hasNow && s.now.title == "Ana" && s.now.start == FETCHED - 600 && s.now.end == FETCHED + 3000, "now");
    check(s.hasNext && s.next.title == "Sam" && s.next.end == FETCHED + 17200, "next");
    check(s.today.size() == 2 && s.today[1].title == "Sam" && s.tomorrow.empty(), "lists");
  }
  {
    // The literal text from the spec, key order and all.
    const std::string spec =
        "{\"instrument\":\"Centrifuge 5424\",\"timezone\":\"America/Detroit\","
        "\"fetched_at\":1792000000,\"status\":\"ok\",\"note\":null,"
        "\"now\":{\"title\":\"Ana\",\"start\":1792000000,\"end\":1792003600},"
        "\"next\":{\"title\":\"Sam\",\"start\":1792010000,\"end\":1792017200},"
        "\"today\":[{\"title\":\"Ana\",\"start\":1792000000,\"end\":1792003600}],\"tomorrow\":[]}";
    reg::Schedule s;
    check(reg::parse(spec, s) && s.today.size() == 1, "the spec's text, verbatim");
  }

  // ---- round trip through the card ---------------------------------------------------------------------------------
  {
    Reply r;
    r.note = "\"1 repeating booking couldn't be read\"";
    r.tomorrow = "[" + entry("Lee \\\"the\\\" Third", FETCHED + 90000, FETCHED + 93600) + "]";
    reg::Schedule a;
    check(reg::parse(r.text(), a), "round trip: reply parses");
    const std::string card = reg::toJson(a);
    check(card.size() <= reg::MAX_CARD_BYTES, "round trip: the card text is bounded");
    reg::Schedule b;
    check(reg::parse(card, b, reg::MAX_CARD_BYTES), "round trip: the card text parses");
    check(b.instrument == a.instrument && b.timezone == a.timezone && b.fetchedAt == a.fetchedAt &&
              b.status == a.status && b.note == a.note && b.hasNow == a.hasNow && b.now == a.now &&
              b.hasNext == a.hasNext && b.next == a.next && b.today == a.today && b.tomorrow == a.tomorrow,
          "round trip: every field comes back");
    check(b.tomorrow[0].title == "Lee \"the\" Third", "round trip: a quote in a title survives");
    check(reg::toJson(b) == card, "round trip: the card text is stable");
  }
  {
    Reply r;
    r.status = "\"unavailable\"";
    r.fetched = "null";
    r.now = "null";
    r.next = "null";
    r.today = "[]";
    r.note = "\"The calendar has not been read yet.\"";
    reg::Schedule a;
    check(reg::parse(r.text(), a) && a.status == reg::Status::Unavailable && a.fetchedAt == 0 && !a.hasNow,
          "an unavailable schedule: no time, no bookings");
    reg::Schedule b;
    check(reg::parse(reg::toJson(a), b, reg::MAX_CARD_BYTES) && b.status == reg::Status::Unavailable, "and it round trips");
  }

  // ---- size ------------------------------------------------------------------------------------------------------
  {
    reg::Schedule s;
    const std::string good = Reply().text();
    check(!reg::parse("", s), "empty text");
    // Padding in the whitespace keeps the text valid JSON while it grows.
    std::string padded = good + std::string(reg::MAX_REPLY_BYTES - good.size(), ' ');
    check(padded.size() == reg::MAX_REPLY_BYTES && reg::parse(padded, s), "exactly MAX_REPLY_BYTES is accepted");
    padded += ' ';
    check(!reg::parse(padded, s), "one byte over MAX_REPLY_BYTES is refused");
    check(reg::parse(padded, s, reg::MAX_CARD_BYTES), "the card limit is larger");
    check(!reg::parse(good + std::string(reg::MAX_CARD_BYTES, ' '), s, reg::MAX_CARD_BYTES), "and it is a limit too");
  }

  // ---- shape -----------------------------------------------------------------------------------------------------
  {
    reg::Schedule s;
    check(!reg::parse("[]", s) && !reg::parse("\"x\"", s) && !reg::parse("null", s) && !reg::parse("42", s),
          "not an object");
    check(!reg::parse("{\"instrument\":", s) && !reg::parse("garbage", s), "not JSON");
    const std::string good = Reply().text();
    check(!reg::parse(good.substr(0, good.size() - 1), s), "cut off");
    for (const char* key : {"instrument", "timezone", "fetched_at", "status", "note", "now", "next", "today", "tomorrow"}) {
      std::string broken = good;
      const std::string quoted = std::string("\"") + key + "\":";
      broken.replace(broken.find(quoted), quoted.size(), std::string("\"x_") + key + "\":");
      check(!reg::parse(broken, s), std::string("a reply without ") + key);
    }
    Reply r;
    r.extra = "\"future\":{\"a\":[1,2]},";
    check(accepts(r), "a key the reader does not know is ignored");
    r = Reply();
    r.now = "{\"title\":\"Ana\",\"start\":1792000000,\"end\":1792003600,\"all_day\":false}";
    check(accepts(r), "so is one in a booking");
    r = Reply();
    r.today = "[[[[[[1]]]]]]";
    check(!accepts(r), "nesting beyond the limit");
  }

  // ---- texts -----------------------------------------------------------------------------------------------------
  {
    Reply r;
    r.instrument = "\"" + repeat("a", 40) + "\"";
    check(accepts(r), "an instrument of 40 characters");
    r.instrument = "\"" + repeat("a", 41) + "\"";
    check(!accepts(r), "41 characters");
    r.instrument = "\"" + repeat("\xc3\xa9", 40) + "\"";  // 40 x e-acute: 80 bytes
    check(accepts(r), "40 two-byte characters");
    r.instrument = "\"" + repeat("\xc3\xa9", 41) + "\"";
    check(!accepts(r), "41 two-byte characters");
    r.instrument = "\"" + repeat("\xf0\x9f\xa7\xaa", 40) + "\"";  // 40 test tubes: 160 bytes
    check(accepts(r), "40 four-byte characters");
    r.instrument = "\"\"";
    check(!accepts(r), "an empty instrument");
    r.instrument = "123";
    check(!accepts(r), "an instrument that is a number");
    r.instrument = "null";
    check(!accepts(r), "an instrument that is null");
    r.instrument = "\"Line\\nbreak\"";
    check(!accepts(r), "a newline in the name");
    r.instrument = "\"Tab\\there\"";
    check(!accepts(r), "a tab in the name");
    r.instrument = "\"Nul\\u0000here\"";
    check(!accepts(r), "a NUL in the name");
    r.instrument = "\"Zero\\u200bwidth\"";
    check(!accepts(r), "a zero-width space");
    r.instrument = "\"Flip\\u202eme\"";
    check(!accepts(r), "a right-to-left override");
    r.instrument = "\"Sep\\u2028arator\"";
    check(!accepts(r), "a line separator");
    r.instrument = "\"Soft\\u00adhyphen\"";
    check(!accepts(r), "a soft hyphen");
    r.instrument = "\"Del\\u007f\"";
    check(!accepts(r), "DEL");
    r.instrument = "\"C1 \\u0085\"";
    check(!accepts(r), "a C1 control");
    r.instrument = "\"Bom\\ufeff\"";
    check(!accepts(r), "a byte order mark");
    r.instrument = "\"Caf\xc3\xa9 \xe2\x80\x94 \xe6\x97\xa5\xe6\x9c\xac\"";
    check(accepts(r), "accents, a dash and CJK are fine");
    r.instrument = "\"Overlong \xc0\x80\"";
    check(!accepts(r), "an overlong NUL");
    r.instrument = "\"Stray \x80 byte\"";
    check(!accepts(r), "a stray continuation byte");
    r.instrument = "\"Cut \xe2\x82\"";
    check(!accepts(r), "a cut-off sequence");
    r.instrument = "\"Surrogate \xed\xa0\x80\"";
    check(!accepts(r), "an encoded surrogate");
    r.instrument = "\"Past \xf4\x90\x80\x80\"";
    check(!accepts(r), "a code point past U+10FFFF");
  }
  {
    Reply r;
    r.today = "[" + entry(repeat("t", 40), FETCHED, FETCHED + 60) + "]";
    check(accepts(r), "a title of 40 characters");
    r.today = "[" + entry(repeat("t", 41), FETCHED, FETCHED + 60) + "]";
    check(dropsOne(r), "a title of 41");
    r.today = "[" + entry("", FETCHED, FETCHED + 60) + "]";
    check(dropsOne(r), "an empty title");
    r.today = "[" + entry("a\\u200fb", FETCHED, FETCHED + 60) + "]";
    check(dropsOne(r), "a right-to-left mark in a title");
    r.today = "[{\"start\":1792000000,\"end\":1792003600}]";
    check(dropsOne(r), "a booking without a title");
    r.today = "[{\"title\":7,\"start\":1792000000,\"end\":1792003600}]";
    check(dropsOne(r), "a title that is a number");
    // The note is taken as it comes: a long one is cut, one that is not text is dropped, and neither costs the schedule.
    reg::Schedule withNote;
    r = Reply();
    r.note = "\"" + repeat("n", 200) + "\"";
    check(reg::parse(r.text(), withNote) && withNote.note == repeat("n", 200), "a note of 200 characters is kept whole");
    r.note = "\"" + repeat("n", 201) + "\"";
    check(reg::parse(r.text(), withNote) && withNote.note == repeat("n", 200), "a note of 201 is cut to 200");
    r.note = "\"" + repeat("\xc3\xa9", 250) + "\"";
    check(reg::parse(r.text(), withNote) && withNote.note == repeat("\xc3\xa9", 200), "a long note is cut between characters, not inside one");
    r.note = "\"The calendar could not be read in time.\"";
    check(reg::parse(r.text(), withNote) && withNote.note == "The calendar could not be read in time.", "the Worker's own sentence");
    r.note = "\"1 repeating booking couldn't be read. Only part of a busy day is shown.\"";
    check(reg::parse(r.text(), withNote) && withNote.note.size() > 60, "two sentences in one note");
    r.note = "\"line\\nbreak\"";
    check(reg::parse(r.text(), withNote) && withNote.note.empty() && withNote.today.size() == 2, "a note with a newline is dropped, the schedule kept");
    r.note = "\"Nul\\u0000here\"";
    check(reg::parse(r.text(), withNote) && withNote.note.empty(), "a NUL in the note: dropped");
    r.note = "\"Zero\\u200bwidth\"";
    check(reg::parse(r.text(), withNote) && withNote.note.empty(), "an invisible character in the note: dropped");
    r.note = "\"Bad \xc0\x80 bytes\"";
    check(reg::parse(r.text(), withNote) && withNote.note.empty(), "malformed UTF-8 in the note: dropped");
    r.note = "\"\"";
    check(reg::parse(r.text(), withNote) && withNote.note.empty(), "an empty note is no note");
    r.note = "5";
    check(!accepts(r), "a note that is a number");
    r.note = "[]";
    check(!accepts(r), "a note that is an array");
    r = Reply();
    r.timezone = "\"Etc/GMT+5\"";
    check(accepts(r), "a zone with a plus");
    r.timezone = "\"America/Port-au-Prince\"";
    check(accepts(r), "a zone with hyphens");
    r.timezone = "\"America/De troit\"";
    check(!accepts(r), "a space in the zone");
    r.timezone = "\"\"";
    check(!accepts(r), "an empty zone");
    r.timezone = "\"" + repeat("z", 65) + "\"";
    check(!accepts(r), "a zone of 65 characters");
    r.timezone = "\"" + repeat("z", 64) + "\"";
    check(accepts(r), "a zone of 64");
    r.timezone = "\"Am\xc3\xa9rica\"";
    check(!accepts(r), "a zone with an accent");
  }

  // ---- status and times ------------------------------------------------------------------------------------------
  {
    Reply r;
    r.status = "\"stale\"";
    reg::Schedule s;
    check(reg::parse(r.text(), s) && s.status == reg::Status::Stale, "stale");
    r.status = "\"fresh\"";
    check(!accepts(r), "an unknown status");
    r.status = "\"OK\"";
    check(!accepts(r), "status is case-sensitive");
    r.status = "1";
    check(!accepts(r), "a status that is a number");
    r = Reply();
    r.fetched = "null";
    check(!accepts(r), "ok without a fetch time");
    r.status = "\"stale\"";
    check(!accepts(r), "stale without a fetch time");
    r = Reply();
    r.status = "\"unavailable\"";
    check(!accepts(r), "unavailable with a fetch time and bookings");
    r.fetched = "null";
    check(!accepts(r), "unavailable with bookings");
    r.now = "null";
    r.next = "null";
    r.today = "[]";
    check(accepts(r), "unavailable with nothing");
    r.today = dayOf(1);
    check(!accepts(r), "unavailable with a booking today");
    r = Reply();
    r.fetched = "\"1792000000\"";
    check(!accepts(r), "a fetch time that is text");
    r.fetched = "1792000000.5";
    check(!accepts(r), "a fetch time with a fraction");
    r.fetched = "1792000000.0";
    check(!accepts(r), "a fetch time written as a float");
    r.fetched = "-5";
    check(!accepts(r), "a negative fetch time");
    r.fetched = "0";
    check(!accepts(r), "a fetch time of zero");
    r.fetched = std::to_string(reg::MIN_EPOCH - 1);
    check(!accepts(r), "a fetch time before 2020");
    r.fetched = std::to_string(reg::MIN_EPOCH);
    check(accepts(r), "a fetch time at 2020");
    r.fetched = std::to_string(reg::MAX_EPOCH);
    check(accepts(r), "a fetch time at 2100");
    r.fetched = std::to_string(static_cast<long long>(reg::MAX_EPOCH) + 1);
    check(!accepts(r), "a fetch time after 2100");
    r.fetched = "4294967296";
    check(!accepts(r), "a fetch time past 32 bits");
    r.fetched = "99999999999999999999";
    check(!accepts(r), "a fetch time that overflows");
  }
  {
    Reply r;
    auto with = [&](long long start, long long end) {
      r.today = "[" + entry("X", start, end) + "]";
      return accepts(r);
    };
    auto dropsWith = [&](long long start, long long end) {
      r.today = "[" + entry("X", start, end) + "]";
      return dropsOne(r);
    };
    check(with(FETCHED, FETCHED + 1), "a booking of one second");
    check(dropsWith(FETCHED, FETCHED), "start == end");
    check(dropsWith(FETCHED + 1, FETCHED), "end before start");
    check(with(FETCHED, FETCHED + 400LL * 86400), "a booking of 400 days");
    // Out of range is clamped and marked, never a reason to refuse the reply.
    {
      reg::Schedule s;
      r.today = "[" + entry("Long", FETCHED, FETCHED + 400LL * 86400 + 1) + "," + entry("Plain", FETCHED + 9, FETCHED + 99) + "]";
      check(reg::parse(r.text(), s) && s.today.size() == 2, "400 days and a second: the reply is kept");
      check(s.today[0].clamped && s.today[0].end == FETCHED + 400LL * 86400 && !s.today[1].clamped, "the long booking is clamped to 400 days and marked, the other is not");
      r.today = "[" + entry("Forever", FETCHED, reg::MAX_EPOCH + 1000LL) + "]";
      check(reg::parse(r.text(), s) && s.today[0].clamped && s.today[0].end == FETCHED + 400LL * 86400, "an end after 2100: clamped and marked");
      r.today = "[" + entry("Forever", FETCHED, 99999999999LL) + "]";
      check(reg::parse(r.text(), s) && s.today[0].clamped, "an end far past 32 bits: clamped");
      r.today = "[" + entry("Since 2019", 1500000000LL, FETCHED + 3600) + "]";
      check(reg::parse(r.text(), s) && s.today.size() == 1 && s.today[0].start == reg::MIN_EPOCH && s.today[0].clamped, "a start before 2020: clamped to 2020, and the 400-day rule marks it");
      r.today = "[" + entry("Short", 1500000000LL, reg::MIN_EPOCH + 3600LL) + "]";
      check(reg::parse(r.text(), s) && s.today[0].start == reg::MIN_EPOCH && !s.today[0].clamped, "a start before 2020 with a short booking: start clamped, not marked");
      r.today = "[" + entry("Ancient", 1000000000LL, 1000003600LL) + "," + entry("Fine", FETCHED, FETCHED + 60) + "]";
      check(reg::parse(r.text(), s) && s.today.size() == 1 && s.today[0].title == "Fine", "a booking wholly before 2020 is dropped, the reply kept");
      r.today = "[" + entry("Far", reg::MAX_EPOCH + 5LL, reg::MAX_EPOCH + 50LL) + "," + entry("Fine", FETCHED, FETCHED + 60) + "]";
      check(reg::parse(r.text(), s) && s.today.size() == 1, "a booking wholly after 2100 is dropped, the reply kept");
      r.now = entry("Ancient", 1000000000LL, 1000003600LL);
      check(reg::parse(r.text(), s) && !s.hasNow, "a dropped 'now' is null");
      r.now = entry("Forever", FETCHED, reg::MAX_EPOCH + 5LL);
      check(reg::parse(r.text(), s) && s.hasNow && s.now.clamped, "a clamped 'now'");
      r = Reply();
    }
    check(dropsWith(-100, -200), "a negative end before its start is malformed");
    check(with(-100, FETCHED), "a negative start is only out of range: clamped");
    r.today = "[{\"title\":\"X\",\"start\":1792000000,\"end\":1792003600,\"clamped\":true}]";
    {
      reg::Schedule s;
      check(reg::parse(r.text(), s) && s.today[0].clamped && s.today[0].end == 1792003600, "the Worker's clamped: true is kept");
      const std::string card = reg::toJson(s);
      check(card.find("\"clamped\":true") != std::string::npos, "the card form says clamped: true");
      reg::Schedule back;
      check(reg::parse(card, back, reg::MAX_CARD_BYTES) && back.today[0].clamped, "clamped survives the card");
    }
    for (const char* bad : {"\"yes\"", "1", "null", "[]"}) {
      r.today = std::string("[{\"title\":\"X\",\"start\":1792000000,\"end\":1792003600,\"clamped\":") + bad + "}]";
      check(dropsOne(r), std::string("clamped must be a boolean: ") + bad);
    }
    r.today = "[{\"title\":\"X\",\"start\":1792000000,\"end\":1792003600,\"clamped\":false}]";
    check(accepts(r), "clamped: false is harmless");
    r.today = "[{\"title\":\"X\",\"start\":\"1792000000\",\"end\":1792003600}]";
    check(dropsOne(r), "a start that is text");
    r.today = "[{\"title\":\"X\",\"start\":1792000000.5,\"end\":1792003600}]";
    check(dropsOne(r), "a start with a fraction");
    r.today = "[{\"title\":\"X\",\"start\":null,\"end\":1792003600}]";
    check(dropsOne(r), "a start that is null");
    r = Reply();
    r.now = "{\"title\":\"X\",\"start\":1792000000,\"end\":1792000000}";
    check(dropsOne(r), "now with start == end");
    r.now = "7";
    check(!accepts(r), "now that is a number");
    r = Reply();
    r.next = "{}";
    check(dropsOne(r), "next that is empty");
  }

  // ---- all-day bookings --------------------------------------------------------------------------------------------
  {
    Reply r;
    r.now = "{\"title\":\"Maintenance\",\"start\":1791936000,\"end\":1792022400,\"all_day\":true}";
    r.today = "[{\"title\":\"Maintenance\",\"start\":1791936000,\"end\":1792022400,\"all_day\":true},{\"title\":\"Sam\",\"start\":1792010000,\"end\":1792017200}]";
    r.tomorrow = "[{\"title\":\"Calibration\",\"start\":1792022400,\"end\":1792108800,\"all_day\":false}]";
    reg::Schedule s;
    check(reg::parse(r.text(), s), "all_day on an entry");
    check(s.now.allDay && s.today[0].allDay && !s.today[1].allDay && !s.tomorrow[0].allDay, "all_day: true marks it, absent and false do not");
    check(!s.now.operator==(reg::Entry{"Maintenance", s.now.start, s.now.end, false}), "an all-day booking is not the same booking as a timed one");
    const std::string card = reg::toJson(s);
    check(card.find("\"all_day\":true") != std::string::npos, "the card form says all_day: true");
    int keys = 0;
    for (size_t at = card.find("\"all_day\""); at != std::string::npos; at = card.find("\"all_day\"", at + 1)) ++keys;
    check(keys == 2 && card.find("\"all_day\":false") == std::string::npos, "and leaves the key out for any other booking (now and one in today: two keys)");
    reg::Schedule back;
    check(reg::parse(card, back, reg::MAX_CARD_BYTES) && back.now.allDay && back.today[0].allDay && !back.today[1].allDay && back.tomorrow[0].start == s.tomorrow[0].start,
          "all_day survives the card");
    for (const char* bad : {"\"yes\"", "1", "0", "null", "[]", "{}", "\"true\""}) {
      r.now = std::string("{\"title\":\"X\",\"start\":1792000000,\"end\":1792003600,\"all_day\":") + bad + "}";
      check(dropsOne(r), std::string("all_day must be a boolean: ") + bad);
    }
  }

  // ---- the instrument's zone -----------------------------------------------------------------------------------------
  {
    const std::string detroit =
        "{\"name\":\"America/Detroit\",\"spans\":[{\"from\":1773000000,\"offset\":-18000},{\"from\":1773122400,\"offset\":-14400},"
        "{\"from\":1793513999,\"offset\":-18000}]}";
    Reply r;
    r.extra = "\"zone\":" + detroit + ",";
    reg::Schedule s;
    check(reg::parse(r.text(), s) && s.hasZone, "a zone is read");
    check(s.zone.name == "America/Detroit" && s.zone.spans.size() == 3 && s.zone.spans[1].offset == -14400 && s.zone.spans[2].from == 1793513999, "its name and spans");
    const std::string card = reg::toJson(s);
    reg::Schedule back;
    check(reg::parse(card, back, reg::MAX_CARD_BYTES) && back.hasZone && back.zone.name == s.zone.name && back.zone.spans == s.zone.spans, "the zone survives the card");
    check(reg::toJson(back) == card, "and the card text is stable");
    check(accepts(Reply()) && !([] { reg::Schedule t; reg::parse(Reply().text(), t); return t.hasZone; })(), "no zone (an older Worker): accepted, hasZone false");
    // A zone that fails any check is dropped; the reply is kept, and the reader's own zone is used.
    const auto dropped = [&](const std::string& zone, const std::string& why) {
      Reply q;
      q.extra = "\"zone\":" + zone + ",";
      reg::Schedule t;
      check(reg::parse(q.text(), t) && !t.hasZone && t.today.size() == 2, "zone dropped, reply kept: " + why);
    };
    dropped("null", "null");
    dropped("[]", "an array");
    dropped("\"America/Detroit\"", "a string");
    dropped("{\"spans\":[{\"from\":1773000000,\"offset\":0}]}", "no name");
    dropped("{\"name\":\"\",\"spans\":[{\"from\":1773000000,\"offset\":0}]}", "an empty name");
    dropped("{\"name\":\"" + repeat("z", 65) + "\",\"spans\":[{\"from\":1773000000,\"offset\":0}]}", "a name of 65 bytes");
    dropped("{\"name\":\"a\\nb\",\"spans\":[{\"from\":1773000000,\"offset\":0}]}", "a newline in the name");
    dropped("{\"name\":\"UTC\"}", "no spans");
    dropped("{\"name\":\"UTC\",\"spans\":[]}", "empty spans");
    dropped("{\"name\":\"UTC\",\"spans\":{}}", "spans that is an object");
    dropped("{\"name\":\"UTC\",\"spans\":[7]}", "a span that is a number");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773122400,\"offset\":0},{\"from\":1773000000,\"offset\":3600}]}", "spans out of order");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773000000,\"offset\":0},{\"from\":1773000000,\"offset\":3600}]}", "two spans from the same time");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773000000,\"offset\":64801}]}", "an offset past +18 h");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773000000,\"offset\":-64801}]}", "an offset past -18 h");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773000000,\"offset\":3600.5}]}", "a fractional offset");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773000000,\"offset\":\"3600\"}]}", "an offset that is text");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":-1,\"offset\":0}]}", "a negative from");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":4102444801,\"offset\":0}]}", "a from after 2100");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":\"1773000000\",\"offset\":0}]}", "a from that is text");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"offset\":0}]}", "a span without from");
    dropped("{\"name\":\"UTC\",\"spans\":[{\"from\":1773000000}]}", "a span without offset");
    std::string nine = "{\"name\":\"UTC\",\"spans\":[";
    std::string eight = nine;
    for (int i = 0; i < 9; ++i) {
      const std::string span = "{\"from\":" + std::to_string(1773000000 + i * 1000) + ",\"offset\":0}";
      nine += (i ? "," : "") + span;
      if (i < 8) eight += (i ? "," : "") + span;
    }
    dropped(nine + "]}", "nine spans");
    Reply e;
    e.extra = "\"zone\":" + eight + "]},";
    check(reg::parse(e.text(), s) && s.hasZone && s.zone.spans.size() == 8, "eight spans are the most");
    Reply edge;
    edge.extra = "\"zone\":{\"name\":\"UTC\",\"spans\":[{\"from\":0,\"offset\":-64800},{\"from\":1,\"offset\":64800}]},";
    check(reg::parse(edge.text(), s) && s.hasZone && s.zone.spans[0].offset == -64800 && s.zone.spans[1].offset == 64800, "offsets of exactly +-18 h and a from of 0 are fine");
  }

  // ---- the card: a power cut between the remove and the rename --------------------------------------------------------
  {
    struct Fake final : reg::CardFiles {
      std::map<std::string, std::string> files;
      bool renameFails = false;
      int renames = 0, removes = 0;
      bool exists(const char* p) override { return files.count(p) != 0; }
      bool read(const char* p, size_t max, std::string& out) override {
        auto it = files.find(p);
        if (it == files.end() || it->second.size() > max) return false;
        out = it->second;
        return true;
      }
      bool rename(const char* tmp, const char* dest) override {
        ++renames;
        if (renameFails) return false;
        files[dest] = files[tmp];
        files.erase(tmp);
        return true;
      }
      bool remove(const char* p) override {
        ++removes;
        return files.erase(p) != 0;
      }
    };
    reg::Schedule a;
    reg::parse(Reply().text(), a);
    const std::string good = reg::toJson(a);
    Reply other;
    other.instrument = "\"Other\"";
    reg::Schedule o;
    reg::parse(other.text(), o);
    const std::string goodOther = reg::toJson(o);
    reg::Schedule got;
    {
      Fake f;
      check(reg::loadCard(f, got) == reg::CardLoad::None, "an empty card: none");
    }
    {
      Fake f;
      f.files[reg::CARD_PATH] = good;
      check(reg::loadCard(f, got) == reg::CardLoad::Main && got.instrument == "Centrifuge 5424" && f.renames == 0, "register.json: used");
    }
    {
      Fake f;
      f.files[reg::CARD_PATH] = good;
      f.files[reg::CARD_TMP_PATH] = goodOther;
      check(reg::loadCard(f, got) == reg::CardLoad::Main && got.instrument == "Centrifuge 5424" && f.files.count(reg::CARD_TMP_PATH) == 1,
            "a register.json that is there is never replaced from the temporary file");
    }
    {
      Fake f;
      f.files[reg::CARD_TMP_PATH] = goodOther;
      check(reg::loadCard(f, got) == reg::CardLoad::Recovered && got.instrument == "Other", "power cut after the remove: the temporary file is used");
      check(f.files.count(reg::CARD_PATH) == 1 && f.files.count(reg::CARD_TMP_PATH) == 0 && f.files[reg::CARD_PATH] == goodOther, "and renamed into place");
      Fake again;
      again.files = f.files;
      check(reg::loadCard(again, got) == reg::CardLoad::Main, "the next boot reads it as the real file");
    }
    {
      Fake f;
      f.files[reg::CARD_TMP_PATH] = goodOther;
      f.renameFails = true;
      check(reg::loadCard(f, got) == reg::CardLoad::Recovered && got.instrument == "Other" && f.files.count(reg::CARD_TMP_PATH) == 0, "a rename that fails: used this boot, temporary file removed");
    }
    {
      Fake f;
      f.files[reg::CARD_TMP_PATH] = goodOther.substr(0, goodOther.size() / 2);
      check(reg::loadCard(f, got) == reg::CardLoad::None && f.files.empty(), "a cut-off temporary file: refused and removed");
    }
    {
      Fake f;
      Reply bad;
      bad.status = "\"weird\"";
      f.files[reg::CARD_TMP_PATH] = bad.text();
      check(reg::loadCard(f, got) == reg::CardLoad::None && f.files.empty(), "a temporary file the parser refuses: removed");
    }
    {
      Fake f;
      f.files[reg::CARD_PATH] = "not json";
      f.files[reg::CARD_TMP_PATH] = goodOther;
      check(reg::loadCard(f, got) == reg::CardLoad::None && f.files.count(reg::CARD_PATH) == 1, "a bad register.json is ignored and left where it is");
    }
  }

  // ---- a reply in the Worker's as-built shape ------------------------------------------------------------------------
  {
    const std::string real =
        "{\"instrument\":\"Zeiss LSM 880\",\"timezone\":\"America/Detroit\","
        "\"zone\":{\"name\":\"America/Detroit\",\"spans\":[{\"from\":1793419200,\"offset\":-14400},{\"from\":1793512800,\"offset\":-18000}]},"
        "\"fetched_at\":1793450000,\"status\":\"ok\",\"note\":\"1 repeating booking couldn't be read.\","
        "\"now\":{\"title\":\"Out of service\",\"start\":1577836800,\"end\":1612396800,\"clamped\":true},"
        "\"next\":{\"title\":\"Sam\",\"start\":1793460000,\"end\":1793467200},"
        "\"today\":[{\"title\":\"Out of service\",\"start\":1577836800,\"end\":1612396800,\"clamped\":true},"
        "{\"title\":\"Maintenance\",\"start\":1793419200,\"end\":1793505600,\"all_day\":true},"
        "{\"title\":\"Booked\",\"start\":1793460000,\"end\":1793467200,\"all_day\":true,\"clamped\":true}],"
        "\"tomorrow\":[]}";
    reg::Schedule s;
    check(reg::parse(real, s), "a reply in the Worker's shape parses");
    check(s.hasZone && s.zone.name == "America/Detroit" && s.zone.spans.size() == 2 && s.zone.spans[1].offset == -18000, "its zone");
    check(s.now.clamped && s.now.start == reg::MIN_EPOCH && s.now.end == 1612396800 && !s.now.allDay, "the clamped entry keeps the Worker's cut times");
    check(s.today.size() == 3 && s.today[1].allDay && !s.today[1].clamped && s.today[2].allDay && s.today[2].clamped, "all_day and clamped together, in either place");
    check(s.note == "1 repeating booking couldn't be read.", "the note");
    const std::string unavailable =
        "{\"instrument\":\"Zeiss LSM 880\",\"timezone\":\"America/Detroit\","
        "\"zone\":{\"name\":\"America/Detroit\",\"spans\":[{\"from\":1793419200,\"offset\":-14400}]},"
        "\"fetched_at\":null,\"status\":\"unavailable\",\"note\":null,\"now\":null,\"next\":null,\"today\":[],\"tomorrow\":[]}";
    check(reg::parse(unavailable, s) && s.status == reg::Status::Unavailable && s.hasZone, "an unavailable reply carries a zone too");
  }

  // ---- counts, order, all or nothing -----------------------------------------------------------------------------
  {
    Reply r;
    r.today = dayOf(20);
    r.tomorrow = dayOf(20);
    reg::Schedule s;
    check(reg::parse(r.text(), s) && s.today.size() == 20 && s.tomorrow.size() == 20, "20 bookings a day");
    check(r.text().size() < 4096 + 1024, "(a full reply with short titles is near the Worker's 4 KB)");
    r.today = dayOf(21);
    check(!accepts(r), "21 bookings today");
    r.today = dayOf(1);
    r.tomorrow = dayOf(21);
    check(!accepts(r), "21 bookings tomorrow");
    r = Reply();
    r.today = "[" + entry("Late", FETCHED + 9000, FETCHED + 9600) + "," + entry("Early", FETCHED + 100, FETCHED + 200) + "," +
             entry("Early", FETCHED + 100, FETCHED + 150) + "]";
    check(reg::parse(r.text(), s) && s.today[0].end == FETCHED + 150 && s.today[1].end == FETCHED + 200 &&
              s.today[2].title == "Late",
          "bookings are put in order of start, then end");
  }
  {
    reg::Schedule s;
    check(reg::parse(Reply().text(), s), "(a good one first)");
    const reg::Schedule before = s;
    Reply bad;
    bad.status = "\"weird\"";
    check(!reg::parse(bad.text(), s), "a bad reply is refused");
    check(s.instrument == before.instrument && s.today == before.today && s.fetchedAt == before.fetchedAt &&
              s.status == before.status,
          "and leaves what was there untouched");
    // A reply whose first half is fine and whose second half is not.
    Reply half;
    half.tomorrow = "[" + entry("ok", FETCHED + 90000, FETCHED + 93600) + ",7]";
    check(!reg::parse(half.text(), s) && s.tomorrow.empty() && s.today.size() == 2, "all or nothing for a structural fault (a booking that is not an object)");
    // A bad booking among good ones costs only itself.
    Reply mixed;
    mixed.today = "[" + entry("ok", FETCHED + 100, FETCHED + 200) + "," + entry("", FETCHED + 1, FETCHED + 2) + "," + entry("zero", FETCHED + 5, FETCHED + 5) + "]";
    reg::Schedule keep;
    check(reg::parse(mixed.text(), keep) && keep.today.size() == 1 && keep.today[0].title == "ok", "one good booking among two bad ones is kept");
    check(keep.note.rfind("2 bookings couldn't be read.", 0) == 0, "and the note says how many could not be read");
    mixed.note = "\"The calendar could not be read in time.\"";
    check(reg::parse(mixed.text(), keep) && keep.note == "2 bookings couldn't be read. The calendar could not be read in time.", "ahead of the Worker's own note");
    mixed.note = "\"" + repeat("n", 200) + "\"";
    check(reg::parse(mixed.text(), keep) && keep.note.rfind("2 bookings", 0) == 0 && keep.note.size() == 200, "the whole note stays within 200 characters");
    reg::Schedule rt;
    check(reg::parse(reg::toJson(keep), rt, reg::MAX_CARD_BYTES) && rt.note == keep.note, "and the card form keeps it as it is");
    Reply nowDropped;
    nowDropped.now = entry("x", FETCHED, FETCHED);
    check(reg::parse(nowDropped.text(), keep) && !keep.hasNow && keep.note.rfind("1 booking", 0) == 0, "a dropped 'now' is null, and counted");
  }

  // ---- the text rule ---------------------------------------------------------------------------------------------
  check(reg::validText("Ana", 40) && !reg::validText("", 40) && !reg::validText("a\x01", 40), "validText");
  check(reg::validText(repeat("a", 5), 5) && !reg::validText(repeat("a", 6), 5), "validText: the limit is in characters");

  // A title or an instrument name of only spaces would draw as a blank: refused (a booking is dropped, the reply is not).
  check(!reg::validText(" ", 40) && !reg::validText("   ", 40) && !reg::validText("\xC2\xA0", 40) &&
            !reg::validText(" \xC2\xA0 \xE3\x80\x80\xE2\x80\x83", 40),
        "validText: only spaces (plain, no-break, em, ideographic) is nothing");
  check(reg::validText(" a ", 40) && reg::validText("\xC2\xA0x", 40),
        "validText: a space beside a real character is a title");
  {
    Reply blankTitle;
    blankTitle.today = "[" + entry("   ", FETCHED + 10000, FETCHED + 17200) + "]";
    check(dropsOne(blankTitle), "a booking titled with spaces is dropped and counted");
    Reply blankInstrument;
    blankInstrument.instrument = "\"   \"";
    check(!accepts(blankInstrument), "an instrument named with spaces: the reply is refused whole");
    Reply blankNow;
    blankNow.now = entry(" ", FETCHED - 600, FETCHED + 3000);
    reg::Schedule kept;
    check(reg::parse(blankNow.text(), kept) && !kept.hasNow, "'now' titled with a space is not a booking");
  }

  std::cout << "register_data: " << checks << " checks passed\n";
  return 0;
}
