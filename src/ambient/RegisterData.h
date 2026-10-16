#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// An instrument's schedule as the mailbox sends it (docs/SPEC-REGISTER.md section 1, GET /api/device/<id>/register),
// and the copy kept on the card. Pure logic with no Arduino and no storage, covered by test/register_data.
//
// A reply from the network and a file from the card are both untrusted: they are read by the same strict parser, and
// a reply that fails any check is thrown away whole (the panel keeps what it shows and goes stale on its own clock).
// The parser tolerates keys it does not know, so the Worker can add fields without breaking readers in the field.
namespace reg {

// The Worker keeps a reply under 4 KB. The reader accepts twice that, and a card file (the same text, written by
// this reader) a little more, so a reply the Worker was allowed to send is never the thing that fails.
constexpr size_t MAX_REPLY_BYTES = 8192;
constexpr size_t MAX_CARD_BYTES = 12288;
constexpr size_t MAX_DAY_ENTRIES = 20;     // per day, as the Worker caps it
constexpr size_t MAX_TITLE_CHARS = 40;     // an instrument name or a booking's title
constexpr size_t MAX_NOTE_CHARS = 200;     // the Worker's plain sentences (an error, what it could not read, a cut day)
constexpr size_t MAX_ZONE_CHARS = 64;      // an IANA name such as America/Detroit
constexpr int NESTING_LIMIT = 4;           // root, "today", an entry, its fields
constexpr uint32_t MIN_EPOCH = 1577836800;  // 2020-01-01: before this a time is not a date this schedule can hold
constexpr uint32_t MAX_EPOCH = 4102444800;  // 2100-01-01
constexpr uint32_t MAX_BOOKING_SECONDS = 400u * 86400u;
constexpr size_t MAX_ZONE_SPANS = 8;       // the Worker sends at most 4
constexpr int32_t MAX_ZONE_OFFSET_S = 18 * 3600;
constexpr size_t MAX_ZONE_NAME_BYTES = 64;

constexpr char CARD_FOLDER[] = "/Lamina";
constexpr char CARD_PATH[] = "/Lamina/register.json";
constexpr char CARD_TMP_PATH[] = "/Lamina/register.json.tmp";

// One booking. `title` is the name the Worker shows for it ("Booked" when names are hidden).
struct Entry {
  std::string title;
  int64_t start = 0;  // Unix seconds
  int64_t end = 0;    // Unix seconds, after start
  bool allDay = false;  // the calendar's all-day event ("all_day": true; the key is absent otherwise)
  // The real end is far away or not known ("clamped": true from the Worker, or an end the reader had to clamp to fit
  // MIN_EPOCH..MAX_EPOCH and MAX_BOOKING_SECONDS): `end` is only a stand-in, and the screen says "until further
  // notice" instead of a time.
  bool clamped = false;
  bool operator==(const Entry& o) const {
    return start == o.start && end == o.end && allDay == o.allDay && clamped == o.clamped && title == o.title;
  }
};

// The instrument's own time zone as the Worker describes it: the offsets in force from the start of today through the end
// of tomorrow and a margin, so the reader draws the instrument's clock whatever its own zone is set to. A span applies
// from its `from` until the next span's `from` (the first one applies to anything earlier, the last to anything later).
struct ZoneSpan {
  int64_t from = 0;     // Unix seconds
  int32_t offset = 0;   // seconds east of UTC
  bool operator==(const ZoneSpan& o) const { return from == o.from && offset == o.offset; }
};
struct Zone {
  std::string name;  // IANA, for the log and the card only
  std::vector<ZoneSpan> spans;
};

// ok: the calendar was read recently. stale: the last attempt failed or the reading is over 30 minutes old (the Worker
// decides). unavailable: it has never been read.
enum class Status : uint8_t { Ok, Stale, Unavailable };

struct Schedule {
  std::string instrument;
  std::string timezone;   // IANA name, informational (the offsets are in `zone`)
  bool hasZone = false;   // the reply carried a valid "zone"; without one (an older Worker) the reader's own zone is used
  Zone zone;
  int64_t fetchedAt = 0;  // when the Worker last read the calendar; 0 only when Unavailable
  Status status = Status::Unavailable;
  std::string note;       // a plain sentence about what could not be read; empty for none
  bool hasNow = false;
  Entry now;              // the booking in progress when the reply was made
  bool hasNext = false;
  Entry next;             // the next booking, up to 7 days ahead
  std::vector<Entry> today;
  std::vector<Entry> tomorrow;
};

// Reads a reply (at most MAX_REPLY_BYTES) or a card file (at most MAX_CARD_BYTES). All or nothing: on false `out` is
// left as it was. Checked, in this order: size, well-formed JSON at most NESTING_LIMIT deep, an object; instrument 1..40 characters; timezone
// 1..64 characters of [A-Za-z0-9_+-/]; status one of ok, stale, unavailable; fetched_at a whole number between
// MIN_EPOCH and MAX_EPOCH (null only for unavailable, which carries no times of any kind); now and next null or a
// booking; today and tomorrow arrays of at most 20 bookings each (an unavailable schedule has none). A booking has a
// title of 1..40 characters, a start and an end that are whole numbers between MIN_EPOCH and MAX_EPOCH with start <
// end and a length of at most 400 days, and optionally "all_day", which must be a boolean. Every text is well-formed
// UTF-8 with no control, invisible format or separator characters. Bookings are put in order of start.
//
// A number that is out of range does not cost the reply: a booking that ends past MAX_EPOCH, or runs longer than
// MAX_BOOKING_SECONDS, or starts before MIN_EPOCH is clamped into range, and the end is marked `clamped`; one that lies
// wholly outside MIN_EPOCH..MAX_EPOCH is dropped. A booking whose end is not after its start, or that is not a whole
// number, is a malformed reply.
//
// The optional "zone" ({"name", "spans": [{"from", "offset"}]}) is read strictly (a name of 1..64 printable bytes; 1..8
// spans in strictly ascending `from`, each between 0 and MAX_EPOCH; offsets within +-18 hours) but is not required: a
// reply without one, or with one that fails any check, is accepted with hasZone false. Keys the reader does not know,
// at the top level or in a booking, are ignored.
//
// The note is the one text taken as it comes: it must be null or a string, but a note that is longer than
// MAX_NOTE_CHARS is cut to that, and one that is not printable text is dropped, because the schedule it comes with is
// worth more than the sentence about it.
bool parse(std::string_view json, Schedule& out, size_t maxBytes = MAX_REPLY_BYTES);

// The schedule in the shape parse() reads, compact, for the card. parse(toJson(s)) gives back s.
std::string toJson(const Schedule& schedule);

// Text a person typed that is safe to draw: 1..max characters, well-formed, nothing unprintable. Exposed for tests.
bool validText(std::string_view text, size_t maxChars);

// The card's two files, as a port so the host test can cut the power between them. `read` fails for a missing or
// unreadable file; `rename` moves the temporary file over the real one (the real one may not exist).
class CardFiles {
 public:
  virtual ~CardFiles() = default;
  virtual bool exists(const char* path) = 0;
  virtual bool read(const char* path, size_t maxBytes, std::string& out) = 0;
  virtual bool rename(const char* tmpPath, const char* path) = 0;
  virtual bool remove(const char* path) = 0;
};
enum class CardLoad : uint8_t {
  None,       // no schedule on the card, or one that fails the parser (left where it is)
  Main,       // /Lamina/register.json
  Recovered,  // the file was missing and the temporary file, written whole before a power cut, was good
};
// Reads the schedule from the card. When register.json is missing and register.json.tmp is there (a power cut between
// the remove and the rename of a save), the temporary file is validated with the same strict parser and moved into
// place; one that fails is removed. A register.json that is there is never replaced from the temporary file.
CardLoad loadCard(CardFiles& card, Schedule& out);

}  // namespace reg
