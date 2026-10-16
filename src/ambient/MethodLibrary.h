#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "MethodPorts.h"
#include "MethodProtocol.h"

// The books on the card: an index of up to 12 (methods, tables, plate maps, boards) in /Lamina/Method/, kept
// in /Lamina/Method/index.json together with the mailbox high-water mark. Pure logic over FilePort, covered
// by test/method_library.
namespace method {

constexpr size_t MAX_METHODS = 12;
// The longest one entry can be in index.json without timers: 480 bytes of title that all need escaping (adopted
// titles are bounded by bytes, not characters, so 480 quotes become 960), a 95-byte path, a 64-byte hash, a
// 20-character book id, a 19-digit time and the fixed keys. Rounded up; test/method_library builds the real worst
// case against it.
constexpr size_t MAX_ENTRY_BYTES = 1280;
// What a book's timers add to its entry: the array the Worker allows (MAX_TIMERS_JSON_BYTES) and its key. Only a book
// the mailbox sent can have timers, and the shelf holds at most MAX_METHODS of those (past that the oldest goes).
constexpr size_t MAX_ENTRY_TIMER_BYTES = MAX_TIMERS_JSON_BYTES + 32;
constexpr size_t INDEX_FRAME_BYTES = 64;  // the version, the mark and the brackets around the entries
// What the index may hold. Past MAX_METHODS the reader prunes only books it downloaded itself, so hand-copied books
// can keep the shelf above 12 and the index must not cut them off the shelf on the next load.
constexpr size_t MAX_INDEXED = 25;
static_assert(MAX_INDEXED >= MAX_METHODS + 8, "room for hand-copied books beyond the usual twelve");
// The largest index this reader writes, which is also the most it will read: an index larger than this reads as
// unreadable. MAX_INDEXED worst-case entries, and the timers that MAX_METHODS of them can carry.
constexpr size_t MAX_INDEX_BYTES =
    INDEX_FRAME_BYTES + MAX_INDEXED * MAX_ENTRY_BYTES + MAX_METHODS * MAX_ENTRY_TIMER_BYTES;
constexpr char INDEX_PATH[] = "/Lamina/Method/index.json";
constexpr char INDEX_TMP_PATH[] = "/Lamina/Method/index.json.tmp";
constexpr char INDEX_BAD_PATH[] = "/Lamina/Method/index.json.bad";  // an index that did not parse, kept for a look
constexpr char LEGACY_STATE_PATH[] = "/.crosspoint/lamina-method.json";

struct Entry {
  std::string title;
  std::string path;    // /Lamina/Method/<name>.xtc
  std::string sha256;  // empty for a book the reader found on the card (never hashed)
  std::string bookId;  // empty for the same reason
  Kind kind = Kind::Method;  // what the mailbox said; a book found on the card is a method
  int64_t receivedAt = 0;  // unix seconds, 0 when unknown
  uint32_t pageCount = 0;  // cover plus steps
  bool opened = false;
  // Timers the mailbox sent with this book, validated against pageCount. Only a book with a SHA-256 (one the
  // mailbox sent) has any; a book found on the card never does.
  std::vector<Timer> timers;
};

class Library {
  std::vector<Entry> entries_;  // newest received first
  uint32_t mark_ = 0;  // the largest mailbox book id the reader has dealt with
  bool readOnly_ = false;  // load() could not read the index on the card: it must not be replaced by a rebuild
  bool outOfMemory_ = false;  // the last parse() ran out of heap, which says nothing about the text

  size_t insertSorted(Entry entry);

 public:
  const std::vector<Entry>& entries() const { return entries_; }
  const Entry* find(std::string_view path) const;
  // True after a load() that found an index on the card it could not read, or could not set aside: save() and
  // commit() refuse, so nothing can be filed this visit and a check would only download a book it cannot keep.
  bool readOnly() const { return readOnly_; }
  // How many books the shelf holds beyond MAX_METHODS because none of the surplus could be pruned.
  size_t overLimit() const { return entries_.size() > MAX_METHODS ? entries_.size() - MAX_METHODS : 0; }
  int indexOf(std::string_view path) const;

  // Replaces the contents from index.json text. False when the text is not an index
  // (the library is then empty). Entries that fail validation are dropped one by one.
  bool parse(std::string_view json);
  // The exact bytes written to index.json.
  std::string serialise() const;

  // Each mutation returns the book files that become garbage once the new index is
  // durable. Nothing is deleted here: see commit().
  //
  // add(): a method with the same title replaces the old entry (new version); different
  // titles are kept; past MAX_METHODS the oldest by receivedAt that the reader downloaded itself (it has a
  // SHA-256) goes, never the new one. A book found on the card (copied there by hand, no SHA-256) is never
  // deleted automatically: it keeps counting, and when only such books are left the shelf stays above
  // MAX_METHODS (see overLimit()).
  std::vector<std::string> add(Entry entry);
  // remove(): the mailbox high-water mark is what keeps a removed book removed (its id is at or below the
  // mark); a re-send from the Mac gets a new id and comes back.
  std::vector<std::string> remove(std::string_view path);
  // Removes a book the way the shelf's "Remove" does it: off the index, the index written, and only then the file,
  // its reading cache and its Recent Books entry. When the index cannot be written the library is put back exactly
  // as it was and nothing on the card changes, so what the shelf shows is still what the card holds.
  enum class RemoveResult : uint8_t { NotListed, Removed, WriteFailed };
  RemoveResult removeAndCommit(FilePort& files, std::string_view path);
  bool markOpened(std::string_view path);  // true when it changed

  // True when the card already holds this version (same SHA-256, or the file name that version would get).
  bool hasVersion(std::string_view sha256, std::string_view path) const;

  // A book sent again with the same bytes brings its timers with it, and they are not part of the bytes: the held
  // version's entry (found by path or SHA-256) takes the new set. True when that changed it. A book found on the card
  // (no SHA-256) never has any.
  bool updateTimers(std::string_view path, std::string_view sha256, const std::vector<Timer>& timers);

  // The largest mailbox book id dealt with. A book is new when its id is above the mark. The mark only
  // moves up through advanceMark; resetMark is for a mailbox that started over.
  uint32_t mark() const { return mark_; }
  void advanceMark(uint32_t id) { mark_ = id > mark_ ? id : mark_; }
  void resetMark() { mark_ = 0; }

  struct LoadReport {
    bool indexRead = false;   // index.json existed and parsed
    bool recovered = false;   // rebuilt from index.json.tmp after an interrupted write
    size_t dropped = 0;       // entries whose file was gone
    size_t adopted = 0;       // .xtc files on the card that were not indexed
    bool migrated = false;    // imported the Stage-0 state file
    size_t opened = 0;        // entries marked opened because the book has saved progress
    bool saved = false;       // index.json was rewritten
    bool indexUnreadable = false;  // index.json exists but could not be read (card error, or over the size cap):
                                   // the shelf is built in memory only, and nothing is written over the file
    bool indexDamaged = false;     // index.json was read but is not an index: kept as index.json.bad, then rebuilt
  };
  // Shelf entry: read the index, import the old state file, reconcile with the folder,
  // pick up progress, and rewrite the index when anything changed.
  LoadReport load(FilePort& files);

  // index.json.tmp, then replace index.json: an interrupted write leaves the old index. The real replace removes
  // the old file and then renames, so a rename that fails leaves the temp file as the only complete copy: it is
  // kept (load() recovers from it) unless the old index is still there. False, with nothing written, after a load()
  // that could not read the index on the card.
  bool save(FilePort& files) const;
  // Writes the index, and only when that succeeded deletes `garbage` (never a path still indexed).
  bool commit(FilePort& files, const std::vector<std::string>& garbage) const;
};

// Finds one book in index.json without parsing, or holding, the rest of the file: the reader opens a book with only
// its path, and an index with timers can be tens of KB on a heap the reader has little of. The index is written one
// entry per line (Library::serialise), so feed() cuts the text into lines as it arrives (any piece size), and only a
// line that names `path` is parsed. A line over MAX_ENTRY_BYTES + MAX_ENTRY_TIMER_BYTES is not an entry this reader
// wrote and is skipped.
class IndexScanner {
 public:
  explicit IndexScanner(std::string_view path);
  // Returns true once the book has been found and read; further input is ignored.
  bool feed(std::string_view chunk);
  // End of the file: a last line with no newline after it.
  bool finish();
  bool found() const { return found_; }
  // The entry as Library::parse would read it (timers dropped by the same rules); valid only when found().
  const Entry& entry() const { return entry_; }

 private:
  void endLine();
  std::string path_;
  std::string needle_;  // "path":"<path>"; empty when the path is not an owned book path
  std::string line_;
  bool tooLong_ = false;
  bool found_ = false;
  Entry entry_;
};

// The kind index.json (given as text) records for the book at `path`, for a screen that only needs to know what
// the book is (the reader's chapter menu: "Steps" or "Pages"). It parses the text and nothing else: no
// reconciling with the card, no writing, no deleting. A method when the text is not an index or does not list
// the path, which is what an unknown book is everywhere else.
Kind indexedKind(std::string_view indexJson, std::string_view path);

// The Stage-0 state file /.crosspoint/lamina-method.json.
struct LegacyBook {
  std::string bookId;
  uint32_t idNumber = 0;
  std::string sha256;
  std::string path;
  std::string title;
};
bool parseLegacyState(std::string_view json, LegacyBook& out);

}  // namespace method
