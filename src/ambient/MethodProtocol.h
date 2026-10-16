#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Wire contract for the mailbox "book" routes (GET /api/device/<id>/book) and
// the SD naming rules for the single downloaded book. Pure logic: no Arduino,
// no storage, so it is covered by the host tests in test/method_book.
namespace method {
constexpr char DIRECTORY[] = "/Lamina/Method";
constexpr char DIRECTORY_PREFIX[] = "/Lamina/Method/";
constexpr char INCOMING_PATH[] = "/Lamina/Method/.incoming.xtc";
constexpr size_t MAX_BOOK_BYTES = 1900000;
constexpr size_t MAX_METADATA_BYTES = 2048;
constexpr size_t MAX_LIST_BOOKS = 12;
constexpr size_t MAX_PAGES = 200;
constexpr size_t MAX_TITLE_CHARS = 120;
constexpr size_t MAX_TITLE_BYTES = 4 * MAX_TITLE_CHARS;  // UTF-8
constexpr size_t MAX_PATH_BYTES = 96;
constexpr size_t SHA256_HEX_BYTES = 64;
constexpr size_t FILE_SHA_CHARS = 8;

// Timers that travel with a book (SPEC-DEVICES section 3): the Mac prints a timer box on a step page and sends where
// it drew it, so the reader can overwrite it with a countdown.
constexpr size_t MAX_TIMERS = 100;
constexpr uint32_t MAX_TIMER_SECONDS = 359999;  // 99:59:59
constexpr size_t MAX_TIMER_LABEL_CHARS = 40;
constexpr size_t MAX_TIMER_LABEL_BYTES = 4 * MAX_TIMER_LABEL_CHARS;  // UTF-8
// The Worker takes at most 4096 bytes of timer data per book.
constexpr size_t MAX_TIMERS_JSON_BYTES = 4096;
constexpr int PAGE_WIDTH_PX = 480;
constexpr int PAGE_HEIGHT_PX = 800;
constexpr int MIN_TIMER_BOX_PX = 24;

// What this parser understands of a /books reply beyond today's shape, as the tokens of the X-Lamina-Features request
// header (SPEC-DEVICES section 3): the Worker lists a book's timers only for a request that names "timers", because an
// older reader's parser would reject the whole list over them. The HTTP adapter sends exactly this.
constexpr char LIST_FEATURES[] = "timers";

// GET /books: at most twelve entries. Without timers the reply is under 12 KB even with twelve 120-character titles;
// the Worker adds up to MAX_TIMERS_JSON_BYTES of timers to each entry, so a full list can reach about 60 KB. The cap is
// 64 KB (SPEC-DEVICES: a reader that asks for timers must accept that much), and a list is nested six deep: the root,
// "books", an entry, "timers", a timer and its "box". The reply is read into one buffer sized from Content-Length: on
// this board an allocation that large is placed in PSRAM (CONFIG_SPIRAM_USE_MALLOC, only blocks under 4 KB stay
// internal), so it does not compete with the TLS session for the internal heap; a list of a few timers per book is a
// few KB either way.
constexpr size_t MAX_LIST_BYTES = 64 * 1024;
constexpr int LIST_NESTING_LIMIT = 7;  // one level beyond the six a list with timers needs

// What a book is for. The Worker sends it with every /books entry (a /book reply has none: a method).
enum class Kind : uint8_t { Method, Table, Plate, Board };
const char* kindName(Kind kind);                // "method", "table", "plate", "board"
bool parseKind(std::string_view name, Kind& out);  // false for any other name
// Only a method is a procedure with steps and progress. A table, plate map or board is reference: it has pages,
// read straight through.
constexpr bool kindHasSteps(Kind kind) { return kind == Kind::Method; }

struct Timer {
  uint16_t page = 0;     // 1-based XTC page, the cover being page 1
  uint32_t seconds = 0;  // 1 to MAX_TIMER_SECONDS
  std::string label;     // 0 to MAX_TIMER_LABEL_CHARS characters; empty when the book gave none
  uint16_t x = 0;        // the box on the page, in page pixels: inside 480 x 800, at least 24 x 24
  uint16_t y = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  bool operator==(const Timer& other) const {
    return page == other.page && seconds == other.seconds && label == other.label && x == other.x && y == other.y &&
           w == other.w && h == other.h;
  }
};

// The timer on 1-based `page`, or null.
const Timer* timerOnPage(const std::vector<Timer>& timers, unsigned page);

struct BookMetadata {
  char bookId[25] = {};
  char title[MAX_TITLE_BYTES + 1] = {};
  char sha256[SHA256_HEX_BYTES + 1] = {};
  char bookUrl[96] = {};
  uint32_t byteLength = 0;
  uint16_t pageCount = 0;
  Kind kind = Kind::Method;
  uint32_t idNumber = 0;  // the number in bookId ("b12" -> 12); ids only ever increase
  bool empty = true;  // {"book_id": null}: the mailbox holds no book
  // The book's timers, all of them or none: a set that fails validation is dropped (the book is kept) and
  // timersDropped says so, so the check can log it.
  std::vector<Timer> timers;
  bool timersDropped = false;
};

// True when `json` is a well-formed reply. `{"book_id": null}` parses with
// result.empty set. Anything else must carry a complete, in-range description
// whose book_url is exactly /api/device/<deviceId>/book/<book_id>, so the
// Bearer token can only ever be sent to the configured origin and route.
bool parseBook(std::string_view json, std::string_view deviceId, BookMetadata& result);

// Reads GET /api/device/<deviceId>/books: {"books": [ ... ]}. Each entry is validated as strictly as
// parseBook validates a /book reply (book_url exactly this device's route for that id, known kind, sane
// sizes). An entry that fails is skipped on its own and counted in `skipped`, so one damaged row cannot
// hide the rest; the reply itself must be an object with a "books" array. `newestId` is the largest id
// among the valid entries (0 when there are none). The list is not reordered. An entry's "timers" are validated
// with it (see parseTimerArray in MethodTimers.h); a set that fails is dropped and the entry kept.
bool parseBooks(std::string_view json, std::string_view deviceId, std::vector<BookMetadata>& out, uint32_t& newestId,
                size_t& skipped);

// "b12" -> 12; false unless it is a valid book id whose number fits 32 bits.
bool bookIdNumber(std::string_view id, uint32_t& out);

// Writes "<sanitized title> <first 8 hex of sha256>.xtc" into out. The title is
// reduced to ASCII letters, digits, space, '-' and '_' and capped in length.
bool bookFileName(std::string_view title, std::string_view sha256, char* out, size_t outSize);

// Writes DIRECTORY_PREFIX + bookFileName(...) into out.
bool bookPath(std::string_view title, std::string_view sha256, char* out, size_t outSize);

// True only for a plain "<DIRECTORY>/<name>.xtc" path with no sub-folder, no
// dot-file name and no ".." (so a damaged state file can never point a delete
// at some other book on the card).
bool ownedBookPath(std::string_view path);

bool validSha256Hex(std::string_view value);  // 64 lowercase hex digits
bool validBookId(std::string_view id);         // "b" and 1 to 19 digits, as the Worker issues them
// True only for the two routes the Bearer token may travel to: "/api/device/<deviceId>/books" (the list)
// and "/api/device/<deviceId>/book/b<digits>" (one book). The check refuses to open anything else; the old
// single-book route "/book" is not one of them.
bool routeAllowed(std::string_view deviceId, std::string_view path);
// The host name of an http(s) origin: "https://mail.example.org:8787/x" -> "mail.example.org". Empty when the text is
// not an http(s) URL, has no host, or the host is a bracketed IPv6 literal.
std::string urlHost(std::string_view url);
// A dotted-quad IPv4 address (four numbers 0 to 255): there is nothing to look up.
bool isIpv4Literal(std::string_view host);
void toHex(const uint8_t digest[32], char out[SHA256_HEX_BYTES + 1]);
// Length in bytes of the longest prefix of text that fits maxBytes without
// splitting a UTF-8 sequence.
size_t utf8Prefix(std::string_view text, size_t maxBytes);
}  // namespace method
