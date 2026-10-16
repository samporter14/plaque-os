#include <functional>
#include <iostream>

#include "FakePorts.h"
#include "MethodTimers.h"

using fake::check;
using fake::checkEq;
using method::Entry;
using method::Library;

namespace {
const std::string SHA_A(64, 'a');
const std::string SHA_B(64, 'b');
const std::string SHA_C(64, 'c');

std::string bookPath(const std::string& name) { return std::string("/Lamina/Method/") + name + ".xtc"; }
const std::string BOOK_BYTES = std::string("XTC\0", 4) + "pages";

Entry entry(const std::string& title, const std::string& name, int64_t receivedAt, const std::string& sha = SHA_A,
            bool opened = false) {
  Entry e;
  e.title = title;
  e.path = bookPath(name);
  e.sha256 = sha;
  e.bookId = "b1";
  e.receivedAt = receivedAt;
  e.pageCount = 8;
  e.opened = opened;
  return e;
}

std::vector<std::string> titles(const Library& library) {
  std::vector<std::string> out;
  for (const auto& e : library.entries()) out.push_back(e.title);
  return out;
}

std::string json(const std::string& entries, const std::string& mark = "0") {
  return "{\"version\":1,\"mark\":" + mark + ",\"methods\":[" + entries + "]}";
}

std::string item(const std::string& title, const std::string& path, const std::string& sha = std::string(64, 'a'),
                 const std::string& pages = "8", const std::string& received = "100", const std::string& opened = "false") {
  return "{\"title\":\"" + title + "\",\"path\":\"" + path + "\",\"sha256\":\"" + sha +
         "\",\"book_id\":\"b3\",\"received_at\":" + received + ",\"page_count\":" + pages + ",\"opened\":" + opened +
         "}";
}

void testSerialiseFormat() {
  Library library;
  check(library.parse(json("")), "empty index parses");
  checkEq(library.serialise(), std::string("{\"version\":1,\"mark\":0,\"methods\":[]}\n"), "empty index format");

  library = Library();
  library.add(entry("Plasmid \"prep\" \\ one", "Plasmid prep 11111111", 200, SHA_B, true));
  library.add(entry("RNA extraction", "Extract 22222222", 100));
  const std::string expected =
      "{\"version\":1,\"mark\":0,\"methods\":[\n"
      "{\"title\":\"Plasmid \\\"prep\\\" \\\\ one\",\"path\":\"/Lamina/Method/Plasmid prep 11111111.xtc\",\"sha256\":\"" +
      SHA_B +
      "\",\"book_id\":\"b1\",\"kind\":\"method\",\"received_at\":200,\"page_count\":8,\"opened\":true},\n"
      "{\"title\":\"RNA extraction\",\"path\":\"/Lamina/Method/Extract 22222222.xtc\",\"sha256\":\"" +
      SHA_A +
      "\",\"book_id\":\"b1\",\"kind\":\"method\",\"received_at\":100,\"page_count\":8,\"opened\":false}\n"
      "]}\n";
  checkEq(library.serialise(), expected, "index format: one entry per line, fixed key order, escaped strings");

  // Round trip, including a micro sign and a control character that must be escaped.
  Library other;
  Entry odd = entry("PCR 50 \xC2\xB5L\x01", "PCR 33333333", 300, SHA_C);
  library.add(odd);
  check(other.parse(library.serialise()), "round trip parses");
  checkEq(other.entries().size(), size_t{3}, "round trip keeps every entry");
  checkEq(other.entries()[0].title, std::string("PCR 50 \xC2\xB5L"), "control characters are cleaned, UTF-8 kept");
  checkEq(other.serialise(), other.serialise(), "serialise is deterministic");
}

void testParseValidation() {
  Library library;
  const std::string good = item("Good", "/Lamina/Method/Good aaaaaaaa.xtc");
  check(library.parse(json(good)), "valid index");
  checkEq(library.entries().size(), size_t{1}, "one entry");
  checkEq(library.entries()[0].pageCount, 8u, "page count read");
  checkEq(library.entries()[0].receivedAt, int64_t{100}, "received_at read");

  check(!library.parse(""), "empty text is not an index");
  checkEq(library.entries().size(), size_t{0}, "a failed parse leaves the library empty");
  check(!library.parse("{"), "truncated json is not an index");
  check(!library.parse("[]"), "an array is not an index");
  check(!library.parse(std::string(method::MAX_INDEX_BYTES + 1, ' ')), "oversized text is not an index");

  // Entries are dropped one by one; the rest survive.
  const std::string mixed = json(
      item("Outside", "/Lamina/Other/x.xtc") + "," + item("Dots", "/Lamina/Method/../x.xtc") + "," +
      item("Hidden", "/Lamina/Method/.incoming.xtc") + "," + item("NotXtc", "/Lamina/Method/x.epub") + "," +
      item("Nested", "/Lamina/Method/sub/x.xtc") + "," + item("BadSha", "/Lamina/Method/BadSha aaaaaaaa.xtc", "xyz") +
      "," + item("NoPages", "/Lamina/Method/NoPages aaaaaaaa.xtc", SHA_A, "0") + "," +
      item("Keeps", "/Lamina/Method/Keeps aaaaaaaa.xtc") + "," +
      "{\"title\":\"NoPath\",\"page_count\":3}" + ",42," +
      item("Keeps", "/Lamina/Method/Keeps again.xtc") + "," +  // same title twice: the first wins
      item("Other", "/Lamina/Method/Keeps aaaaaaaa.xtc"));      // same path twice: the first wins
  check(library.parse(mixed), "mixed index parses");
  checkEq(library.entries().size(), size_t{1}, "only the valid, unique entry survives");
  checkEq(library.entries()[0].path, bookPath("Keeps aaaaaaaa"), "the first occurrence wins");

  // A blank title falls back to the file name without its hash.
  check(library.parse(json(item("   ", "/Lamina/Method/Blank title 1a2b3c4d.xtc"))), "blank title parses");
  checkEq(library.entries()[0].title, std::string("Blank title"), "blank title comes from the file name");

  // Sorted newest first, whatever order the file lists them in. Past 12 are kept too (hand-copied books can hold
  // the shelf above the limit) up to the index's own cap, which is only a guard against a runaway file.
  std::string many;
  for (int i = 0; i < 15; ++i)
    many += (i ? "," : "") + item("T" + std::to_string(i), "/Lamina/Method/T" + std::to_string(i) + " aaaaaaaa.xtc",
                                  SHA_A, "5", std::to_string(i));
  check(library.parse(json(many)), "fifteen entries parse");
  checkEq(library.entries().size(), size_t{15}, "an index above 12 is not cut to 12");
  checkEq(library.entries().front().title, std::string("T14"), "newest first");
  checkEq(library.entries().back().title, std::string("T0"), "the oldest is kept");
  std::string runaway;
  for (int i = 0; i < 60; ++i)
    runaway += (i ? "," : "") + item("R" + std::to_string(i), "/Lamina/Method/R" + std::to_string(i) + " aaaaaaaa.xtc",
                                     SHA_A, "5", std::to_string(i));
  check(library.parse(json(runaway)), "sixty entries parse");
  checkEq(library.entries().size(), method::MAX_INDEXED, "a runaway index is cut to its cap");
  checkEq(library.entries().front().title, std::string("R59"), "keeping the newest");

}

// The card library reads a zero-byte file as a failure, like a missing one (HalStorage::readFileToString). An empty
// index.json is therefore not "unreadable" (a card error that must be left alone): it is nothing, a damaged index,
// and the shelf rebuilds it. Anything else that fails to read is still left untouched.
void testEmptyIndexIsDamagedNotUnreadable() {
  fake::FakeFiles files;
  files.put(method::INDEX_PATH, "");
  files.put(bookPath("Stray 11111111"), BOOK_BYTES, 50);
  std::string out;
  check(!files.readAll(method::INDEX_PATH, method::MAX_INDEX_BYTES, out), "precondition: an empty file does not read");
  checkEq(files.fileSize(method::INDEX_PATH), int64_t{0}, "and has size zero");

  Library library;
  const auto report = library.load(files);
  check(report.indexDamaged && !report.indexUnreadable, "an empty index is damaged, not unreadable");
  check(!library.readOnly(), "so the library is not read-only");
  check(report.saved && report.adopted == 1, "the shelf is rebuilt from the folder and the index written");
  check(files.has(method::INDEX_BAD_PATH), "the empty file was set aside like any damaged index");
  check(files.files[method::INDEX_PATH] == library.serialise() && !library.serialise().empty(), "a valid index is back");
  check(library.save(files), "and saving works");

  // An index that is there but fails to read for another reason is still left alone.
  fake::FakeFiles card;
  card.put(method::INDEX_PATH, "{\"version\":1,\"mark\":3,\"methods\":[]}\n");
  card.failRead.insert(method::INDEX_PATH);
  Library other;
  const auto otherReport = other.load(card);
  check(otherReport.indexUnreadable && !otherReport.indexDamaged && other.readOnly(), "a read failure is unreadable");
}

// load() with only the temp file left: it is the only complete copy until it is in place.
void testTempFileRecovery() {
  Library seed;
  Entry kept = entry("Survivor", "Survivor 11111111", 100);
  kept.kind = method::Kind::Table;
  seed.add(kept);
  seed.advanceMark(9);
  const std::string tmpText = seed.serialise();
  const auto card = [&](fake::FakeFiles& files) {
    files.put(bookPath("Survivor 11111111"), BOOK_BYTES);
    files.put(method::INDEX_TMP_PATH, tmpText);
  };
  {
    // A write failure after the read cannot cost the copy: it is renamed into place, not written again.
    fake::FakeFiles files;
    card(files);
    files.failWrite.insert(method::INDEX_TMP_PATH);
    Library library;
    const auto report = library.load(files);
    check(report.recovered && report.saved && !library.readOnly(), "recovered and put back");
    checkEq(files.files[method::INDEX_PATH], tmpText, "the index on the card is the recovered copy, byte for byte");
    check(!files.has(method::INDEX_TMP_PATH), "and the temp file is gone once it is the index");
    check(library.find(bookPath("Survivor 11111111"))->kind == method::Kind::Table && library.mark() == 9u,
          "with the kind and the mark");
  }
  {
    // The rename fails: the temp file stays, the shelf is read-only this visit, and the next load recovers.
    fake::FakeFiles files;
    card(files);
    files.failRenameDest.insert(method::INDEX_PATH);
    Library library;
    const auto report = library.load(files);
    check(report.recovered && !report.saved && library.readOnly(), "recovered in memory only");
    check(files.has(method::INDEX_TMP_PATH) && files.files[method::INDEX_TMP_PATH] == tmpText, "the temp file is kept");
    check(!library.save(files), "nothing is written over it");
    files.failRenameDest.clear();
    Library again;
    check(again.load(files).recovered && !again.readOnly() && files.files[method::INDEX_PATH] == tmpText,
          "the next load puts it back");
  }
  {
    // The temp file itself cannot be read: it may be the only copy, so it is neither deleted nor overwritten.
    fake::FakeFiles files;
    card(files);
    files.failRead.insert(method::INDEX_TMP_PATH);
    Library library;
    const auto report = library.load(files);
    check(report.indexUnreadable && library.readOnly() && !report.recovered, "unreadable");
    check(files.has(method::INDEX_TMP_PATH) && files.files[method::INDEX_TMP_PATH] == tmpText && !files.has(method::INDEX_PATH),
          "the temp file is left exactly as it was");
    check(!library.save(files), "and nothing is written");
  }
  {
    // An empty or torn temp file with no index is worth nothing: removed, and the shelf rebuilds from the folder.
    fake::FakeFiles empty;
    empty.put(method::INDEX_TMP_PATH, "");
    empty.put(bookPath("Stray 22222222"), BOOK_BYTES, 5);
    Library fromFolder;
    const auto report = fromFolder.load(empty);
    check(!report.indexUnreadable && !fromFolder.readOnly() && report.adopted == 1, "an empty temp file is just removed");
    check(!empty.has(method::INDEX_TMP_PATH) || empty.files[method::INDEX_TMP_PATH] == fromFolder.serialise(), "and not kept");

    fake::FakeFiles torn;
    torn.put(method::INDEX_TMP_PATH, "{\"version\":1,\"methods\":[{\"title\":");
    Library tornLibrary;
    check(!tornLibrary.load(torn).indexUnreadable && !tornLibrary.readOnly(), "a torn temp file is not an error either");
  }
}

// The largest index the reader can write must be one it can read back: an index over MAX_INDEX_BYTES reads as
// unreadable, which would make the shelf read-only for good.
void testWorstCaseIndexFitsTheReadCap() {
  const std::string quotes(method::MAX_TITLE_BYTES, '"');  // 480 bytes that each need an escape
  const auto worst = [&](size_t n) {
    Entry e;
    e.title = quotes;
    e.title[0] = static_cast<char>('a' + n % 26);
    e.title[1] = static_cast<char>('a' + n / 26);
    e.path = "/Lamina/Method/" + std::to_string(100 + n) + std::string(73, 'a') + ".xtc";  // 95 bytes
    e.sha256 = SHA_A;
    e.bookId = "b9999999999999999999";  // b and 19 digits
    e.kind = method::Kind::Method;
    e.receivedAt = INT64_MAX;
    e.pageCount = 65535;
    e.opened = false;
    return e;
  };
  check(method::ownedBookPath(worst(0).path) && worst(0).path.size() == 95, "the worst-case path is the longest owned path");
  Library one;
  one.advanceMark(UINT32_MAX);
  const std::string empty = one.serialise();
  one.add(worst(0));
  const size_t perEntry = one.serialise().size() - empty.size();
  check(perEntry <= method::MAX_ENTRY_BYTES, "the worst entry fits MAX_ENTRY_BYTES");
  check(perEntry + 200 > method::MAX_ENTRY_BYTES, "and the bound is not slack");
  check(empty.size() <= method::INDEX_FRAME_BYTES, "the frame fits INDEX_FRAME_BYTES");
  check(method::MAX_INDEXED >= method::MAX_METHODS, "the cap leaves room for the usual twelve");
  // The most timers an entry can carry: as many 40-character labels, all quotes, as the Worker's 4096 bytes hold.
  std::vector<method::Timer> worstTimers;
  while (true) {
    method::Timer t;
    t.page = static_cast<uint16_t>(worstTimers.size() + 1);
    t.seconds = 359999;
    t.label = std::string(40, '"');
    t.x = 456;
    t.y = 776;
    t.w = 24;
    t.h = 24;
    worstTimers.push_back(t);
    std::string probe;
    method::appendTimersJson(probe, worstTimers);
    if (probe.size() > method::MAX_TIMERS_JSON_BYTES) {
      worstTimers.pop_back();
      break;
    }
  }
  std::string worstText;
  method::appendTimersJson(worstText, worstTimers);
  const size_t timerCost = worstText.size() + std::string(",\"timers\":").size();
  check(worstText.size() <= method::MAX_TIMERS_JSON_BYTES && worstText.size() + 200 > method::MAX_TIMERS_JSON_BYTES,
        "the worst timers fill the Worker's 4096 bytes");
  check(timerCost <= method::MAX_ENTRY_TIMER_BYTES && timerCost + 200 > method::MAX_ENTRY_TIMER_BYTES,
        "and the entry's timer allowance is no looser than that");
  // MAX_INDEXED worst entries, MAX_METHODS of them carrying the worst timers: the cap holds exactly that.
  const size_t worstIndex = empty.size() + method::MAX_INDEXED * perEntry + method::MAX_METHODS * timerCost;
  check(worstIndex <= method::MAX_INDEX_BYTES, "a full index of worst entries and timers fits the cap");
  check(worstIndex + 3000 > method::MAX_INDEX_BYTES, "and the cap is no more than the rounding of the bounds above it");

  // Written the way the library writes it (with the largest time a reader can read back), then loaded through the
  // card: every entry comes back, and the file is under what readAll will read.
  std::string text = "{\"version\":1,\"mark\":4294967295,\"methods\":[";
  fake::FakeFiles files;
  for (size_t i = 0; i < method::MAX_INDEXED; ++i) {
    const Entry e = worst(i);
    std::string escaped;
    for (const char c : e.title) escaped += c == '"' ? std::string("\\\"") : std::string(1, c);
    text += (i ? ",\n" : "\n") + std::string("{\"title\":\"") + escaped + "\",\"path\":\"" + e.path +
            "\",\"sha256\":\"" + e.sha256 + "\",\"book_id\":\"" + e.bookId +
            "\",\"kind\":\"method\",\"received_at\":4294967295,\"page_count\":65535,\"opened\":false" +
            (i < method::MAX_METHODS ? ",\"timers\":" + worstText : std::string()) + "}";
    files.put(e.path, BOOK_BYTES);
  }
  text += "\n]}\n";
  check(text.size() <= method::MAX_INDEX_BYTES, "the index text is under the read cap");
  files.put(method::INDEX_PATH, text);
  Library library;
  const auto report = library.load(files);
  check(report.indexRead && !report.indexUnreadable && !library.readOnly(), "it reads back");
  checkEq(library.entries().size(), method::MAX_INDEXED, "with every entry");
  check(library.serialise().size() <= method::MAX_INDEX_BYTES, "and what it writes next is under the cap too");
  size_t carrying = 0;
  for (const Entry& e : library.entries()) carrying += e.timers.size() == worstTimers.size();
  checkEq(carrying, method::MAX_METHODS, "the timers of the twelve books that carry them come back");
}

// readOnly() follows the load: set while an index on the card cannot be read, clear when the next load reads it.
void testReadOnlyFollowsTheLoad() {
  fake::FakeFiles files;
  Library seed;
  seed.add(entry("A", "A 11111111", 100));
  files.put(bookPath("A 11111111"), BOOK_BYTES);
  seed.save(files);
  Library library;
  check(!library.readOnly(), "a new library is writable");
  check(library.load(files).indexRead && !library.readOnly(), "a readable index loads writable");
  files.failRead.insert(method::INDEX_PATH);
  check(library.load(files).indexUnreadable && library.readOnly(), "an unreadable one loads read-only");
  files.failRead.clear();
  check(library.load(files).indexRead && !library.readOnly(), "and the next load is writable again");
}

void testMarkAndKind() {
  // The high-water mark and the kind are part of the index.
  Library library;
  check(library.parse(json("", "42")), "mark parses");
  checkEq(library.mark(), 42u, "mark read");
  check(library.parse("{\"version\":1,\"methods\":[]}"), "an older index without a mark parses");
  checkEq(library.mark(), 0u, "no mark reads as 0");
  check(library.parse(json("", "-3")) && library.mark() == 0, "a negative mark is ignored");
  check(library.parse(json("", "\"7\"")) && library.mark() == 0, "a non-numeric mark is ignored");

  library = Library();
  library.advanceMark(5);
  library.advanceMark(3);
  checkEq(library.mark(), 5u, "the mark only moves up");
  library.advanceMark(9);
  checkEq(library.mark(), 9u, "and up again");
  Entry table = entry("Seeding sheet", "Seeding 11111111", 100, SHA_B);
  table.kind = method::Kind::Table;
  library.add(table);
  Entry plate = entry("Plate A", "Plate 22222222", 90, SHA_C);
  plate.kind = method::Kind::Plate;
  library.add(plate);
  Library other;
  check(other.parse(library.serialise()), "the index round trips");
  checkEq(other.mark(), 9u, "mark survives the round trip");
  check(other.find(bookPath("Seeding 11111111"))->kind == method::Kind::Table, "table kind survives");
  check(other.find(bookPath("Plate 22222222"))->kind == method::Kind::Plate, "plate kind survives");
  check(library.serialise().find("\"kind\":\"table\"") != std::string::npos, "kind is written by name");
  library.resetMark();
  checkEq(library.mark(), 0u, "a mailbox that started over resets the mark");

  // Unknown or missing kinds read as methods; adopted books are methods.
  check(library.parse(json("{\"title\":\"A\",\"path\":\"/Lamina/Method/A aaaaaaaa.xtc\",\"page_count\":3,\"kind\":\"sheet\"},"
                           "{\"title\":\"B\",\"path\":\"/Lamina/Method/B aaaaaaaa.xtc\",\"page_count\":3}")),
        "kinds parse");
  check(library.entries().size() == 2 && library.entries()[0].kind == method::Kind::Method &&
            library.entries()[1].kind == method::Kind::Method,
        "unknown and missing kinds are methods");

  // Removing a book leaves the mark alone: the mark is what keeps it removed.
  Library removing;
  removing.advanceMark(12);
  removing.add(entry("Gone", "Gone 11111111", 100, SHA_A));
  removing.remove(bookPath("Gone 11111111"));
  checkEq(removing.mark(), 12u, "removing does not change the mark");

  // A prune or replacement keeps the mark too.
  Library replacing;
  replacing.advanceMark(30);
  replacing.add(entry("T", "T 11111111", 100, SHA_A));
  replacing.add(entry("T", "T 22222222", 200, SHA_B));
  checkEq(replacing.mark(), 30u, "replacing does not change the mark");

  fake::FakeFiles files;
  Library saved;
  saved.advanceMark(77);
  check(saved.save(files), "save with a mark");
  Library loaded;
  loaded.load(files);
  checkEq(loaded.mark(), 77u, "the mark is read back from the card");
}

void testAddReplaceAndPrune() {
  Library library;
  // Different titles are kept, newest first.
  check(library.add(entry("A", "A 11111111", 100)).empty(), "first add deletes nothing");
  check(library.add(entry("B", "B 22222222", 300)).empty(), "different title deletes nothing");
  check(library.add(entry("C", "C 33333333", 200)).empty(), "another title deletes nothing");
  checkEq(titles(library), (std::vector<std::string>{"B", "C", "A"}), "kept and ordered by received_at");

  // Same title, new version: the entry is replaced and the old file is the garbage.
  const auto garbage = library.add(entry("C", "C 44444444", 400, SHA_B));
  checkEq(garbage, (std::vector<std::string>{bookPath("C 33333333")}), "the replaced version is garbage");
  checkEq(titles(library), (std::vector<std::string>{"C", "B", "A"}), "the new version moved to the top");
  check(!library.find(bookPath("C 33333333")), "the old path is gone from the index");
  check(!library.find(bookPath("C 44444444"))->opened, "a new version starts as new");

  // The same file again (same path) replaces without calling the file garbage.
  check(library.add(entry("C", "C 44444444", 500, SHA_B)).empty(), "same path is not garbage");
  checkEq(library.entries().size(), size_t{3}, "still three methods");

  // Equal timestamps: the newer arrival sorts first.
  Library ties;
  ties.add(entry("First", "First 11111111", 100));
  ties.add(entry("Second", "Second 22222222", 100));
  checkEq(titles(ties), (std::vector<std::string>{"Second", "First"}), "ties put the new arrival on top");

  // Prune at 12: the oldest by received_at goes, and only after commit.
  Library full;
  for (int i = 0; i < 12; ++i) full.add(entry("M" + std::to_string(i), "M" + std::to_string(i) + " 11111111", 1000 + i));
  checkEq(full.entries().size(), size_t{12}, "twelve methods fit");
  const auto pruned = full.add(entry("M12", "M12 11111111", 2000));
  checkEq(full.entries().size(), size_t{12}, "still twelve after the thirteenth");
  checkEq(pruned, (std::vector<std::string>{bookPath("M0 11111111")}), "the oldest is pruned");
  checkEq(full.entries().front().title, std::string("M12"), "the thirteenth is on top");
  check(!full.find(bookPath("M0 11111111")), "M0 is gone");

  // A method that arrives with an old (or unknown) timestamp is never pruned itself.
  const auto prunedOther = full.add(entry("Old arrival", "Old 11111111", 0));
  checkEq(full.entries().size(), size_t{12}, "twelve after an old arrival");
  check(full.find(bookPath("Old 11111111")) != nullptr, "the new method is never the one pruned");
  checkEq(prunedOther, (std::vector<std::string>{bookPath("M1 11111111")}), "the oldest of the others is pruned");

  // Books found on the card (no SHA-256) are never pruned. The oldest the reader downloaded itself goes instead.
  const auto adopted = [](const std::string& title, const std::string& name, int64_t at) {
    Entry e = entry(title, name, at);
    e.sha256.clear();
    e.bookId.clear();
    return e;
  };
  Library mixedShelf;
  mixedShelf.add(adopted("Copied oldest", "Copied oldest 11111111", 1));
  for (int i = 0; i < 11; ++i)
    mixedShelf.add(entry("D" + std::to_string(i), "D" + std::to_string(i) + " 11111111", 100 + i));
  checkEq(mixedShelf.entries().size(), size_t{12}, "twelve with a hand-copied one at the bottom");
  const auto prunedMixed = mixedShelf.add(entry("D11", "D11 11111111", 500));
  checkEq(prunedMixed, (std::vector<std::string>{bookPath("D0 11111111")}),
          "the oldest downloaded book is pruned, not the older hand-copied one");
  check(mixedShelf.find(bookPath("Copied oldest 11111111")) != nullptr, "the hand-copied book is still on the shelf");
  checkEq(mixedShelf.entries().size(), size_t{12}, "still twelve");
  checkEq(mixedShelf.overLimit(), size_t{0}, "nothing over the limit");

  // Only hand-copied books: nothing is pruned, the shelf goes above 12 and says so.
  Library copied;
  for (int i = 0; i < 12; ++i)
    copied.add(adopted("C" + std::to_string(i), "C" + std::to_string(i) + " 11111111", 10 + i));
  const auto prunedCopied = copied.add(entry("Downloaded", "Downloaded 22222222", 900));
  check(prunedCopied.empty(), "no hand-copied book is ever garbage");
  checkEq(copied.entries().size(), size_t{13}, "all thirteen stay");
  checkEq(copied.overLimit(), size_t{1}, "and the shelf reports the surplus");
  check(copied.find(bookPath("C0 11111111")) != nullptr, "the oldest hand-copied book is still there");
  // The next download is the only candidate besides the new one: the older download goes.
  const auto nextPruned = copied.add(entry("Another", "Another 33333333", 1000));
  checkEq(nextPruned, (std::vector<std::string>{bookPath("Downloaded 22222222")}),
          "the surplus is pruned from the downloaded books");
  checkEq(copied.entries().size(), size_t{13}, "so the shelf does not grow with each download");

  // A new version of a hand-copied book's title replaces it (the owner sent a newer one): that is not pruning.
  const auto replacedCopy = copied.add(entry("C3", "C3 44444444", 2000));
  check(std::find(replacedCopy.begin(), replacedCopy.end(), bookPath("C3 11111111")) != replacedCopy.end(),
        "a same-title replacement still makes the old copy garbage");
  checkEq(replacedCopy.size(), size_t{2}, "and the surplus download goes with it");
  check(std::find(replacedCopy.begin(), replacedCopy.end(), bookPath("Another 33333333")) != replacedCopy.end(),
        "(the downloaded one, never another hand-copied book)");

  // A replacement and a prune in the same add: both are garbage.
  Library both;
  for (int i = 0; i < 12; ++i) both.add(entry("N" + std::to_string(i), "N" + std::to_string(i) + " 11111111", 10 + i));
  const auto mixed = both.add(entry("N5", "N5 22222222", 100));
  checkEq(both.entries().size(), size_t{12}, "replacement keeps twelve");
  checkEq(mixed, (std::vector<std::string>{bookPath("N5 11111111")}), "replacing frees a slot, so nothing else is pruned");
}

void testRemove() {
  Library library;
  library.add(entry("A", "A 11111111", 100, SHA_A));
  library.add(entry("B", "B 22222222", 200, SHA_B));
  const auto garbage = library.remove(bookPath("A 11111111"));
  checkEq(garbage, (std::vector<std::string>{bookPath("A 11111111")}), "removing returns the file");
  check(library.remove("/Lamina/Method/nothing.xtc").empty(), "removing an unknown path does nothing");
  checkEq(titles(library), (std::vector<std::string>{"B"}), "the other method stays");
  check(!library.hasVersion(SHA_A, bookPath("A 11111111")), "a removed version is no longer held");
}

// "Remove" on the shelf: the index first, the file after. If the index cannot be written, nothing changes, in memory
// or on the card.
void testRemoveAndCommit() {
  using Result = Library::RemoveResult;
  const auto shelf = [](fake::FakeFiles& files) {
    Library library;
    Entry a = entry("A", "A 11111111", 100, SHA_A);
    a.kind = method::Kind::Table;
    library.add(a);
    library.add(entry("B", "B 22222222", 200, SHA_B));
    library.advanceMark(7);
    files.put(bookPath("A 11111111"), BOOK_BYTES);
    files.put(bookPath("B 22222222"), BOOK_BYTES);
    library.save(files);
    files.progress[bookPath("A 11111111")] = 2;
    files.log.clear();
    return library;
  };
  {
    fake::FakeFiles files;
    Library library = shelf(files);
    check(library.removeAndCommit(files, bookPath("A 11111111")) == Result::Removed, "removed");
    checkEq(titles(library), (std::vector<std::string>{"B"}), "off the shelf");
    check(!files.has(bookPath("A 11111111")), "the file is gone");
    check(files.indexOfLog("replace /Lamina/Method/index.json.tmp /Lamina/Method/index.json") <
              files.indexOfLog("removeBook " + bookPath("A 11111111")),
          "after the index was written");
    Library onCard;
    check(onCard.parse(files.files[method::INDEX_PATH]) && titles(onCard) == titles(library), "and the card agrees");
    checkEq(library.mark(), 7u, "the mark stays: a removed book stays removed");
  }
  {
    fake::FakeFiles files;
    Library library = shelf(files);
    const std::string before = library.serialise();
    check(library.removeAndCommit(files, bookPath("Nothing 99999999")) == Result::NotListed, "an unlisted path");
    checkEq(library.serialise(), before, "changes nothing");
    check(files.log.empty(), "and touches nothing on the card");
  }
  // The index cannot be written (temp write fails, replace fails, rename fails, the index was unreadable): the book
  // stays on the shelf and on the card, and the library is exactly what it was.
  const auto failure = [&](const char* name, const std::function<void(fake::FakeFiles&)>& break_) {
    fake::FakeFiles files;
    Library library = shelf(files);
    const std::string before = library.serialise();
    const std::string indexOnCard = files.files[method::INDEX_PATH];
    break_(files);
    const auto outcome = library.removeAndCommit(files, bookPath("A 11111111"));
    const std::string n = name;
    check(outcome == Result::WriteFailed, (n + ": reported").c_str());
    checkEq(library.serialise(), before, (n + ": the library is unchanged").c_str());
    check(library.find(bookPath("A 11111111")) != nullptr && library.find(bookPath("A 11111111"))->kind == method::Kind::Table,
          (n + ": the book is still listed").c_str());
    check(files.has(bookPath("A 11111111")) && files.progress.count(bookPath("A 11111111")) == 1,
          (n + ": the file and its progress are still on the card").c_str());
    checkEq(files.count("removeBook " + bookPath("A 11111111")), 0, (n + ": nothing was deleted").c_str());
    return files;
  };
  {
    auto files = failure("temp write fails", [](fake::FakeFiles& f) { f.failWrite.insert(method::INDEX_TMP_PATH); });
    checkEq(files.files[method::INDEX_PATH].empty(), false, "the index on the card is still there");
  }
  failure("replace fails", [](fake::FakeFiles& f) { f.failReplaceDest.insert(method::INDEX_PATH); });
  failure("rename fails", [](fake::FakeFiles& f) { f.failRenameDest.insert(method::INDEX_PATH); });
  failure("no folder", [](fake::FakeFiles& f) { f.failEnsure = true; });
}

void testHasVersion() {
  Library library;
  library.add(entry("A", "A 11111111", 100, SHA_A));
  library.add(entry("Found", "Found 22222222", 100, ""));
  check(library.hasVersion(SHA_A, bookPath("A 99999999")), "same hash");
  check(library.hasVersion(SHA_B, bookPath("A 11111111")), "same file name");
  check(library.hasVersion(SHA_B, bookPath("Found 22222222")), "a found book matches by file name");
  check(!library.hasVersion(SHA_B, bookPath("A 33333333")), "a new version is not held");
  check(!library.hasVersion("", bookPath("Zed 11111111")), "an empty hash matches nothing");
}

void testAtomicSave() {
  fake::FakeFiles files;
  Library library;
  library.add(entry("A", "A 11111111", 100));
  files.put(bookPath("A 11111111"), BOOK_BYTES);

  check(library.save(files), "save succeeds");
  const size_t write = files.indexOfLog("write /Lamina/Method/index.json.tmp");
  const size_t replace = files.indexOfLog("replace /Lamina/Method/index.json.tmp /Lamina/Method/index.json");
  check(write != SIZE_MAX && replace != SIZE_MAX && write < replace, "write the temp file, then replace the index");
  check(files.has(method::INDEX_PATH) && !files.has(method::INDEX_TMP_PATH), "the index exists and the temp file is gone");
  checkEq(files.files[method::INDEX_PATH], library.serialise(), "the file holds the serialised library");

  // A failed temp write leaves the old index in place.
  const std::string before = files.files[method::INDEX_PATH];
  Library changed;
  changed.add(entry("Z", "Z 99999999", 5));
  files.failWrite.insert(method::INDEX_TMP_PATH);
  check(!changed.save(files), "a failed temp write fails the save");
  checkEq(files.files[method::INDEX_PATH], before, "the old index is untouched");
  check(!files.has(method::INDEX_TMP_PATH), "no temp file is left behind");
  files.failWrite.clear();

  // A failed replace also leaves the old index in place and cleans up.
  files.failReplaceDest.insert(method::INDEX_PATH);
  check(!changed.save(files), "a failed replace fails the save");
  checkEq(files.files[method::INDEX_PATH], before, "the old index survives a failed replace");
  check(!files.has(method::INDEX_TMP_PATH), "the temp file is removed after a failed replace");
  files.failReplaceDest.clear();

  // The card's replace removes the old file and then renames. If the rename fails the temp file is the only
  // complete copy: it stays, and the next load recovers from it.
  files.failRenameDest.insert(method::INDEX_PATH);
  check(!changed.save(files), "a failed rename fails the save");
  check(!files.has(method::INDEX_PATH), "the old index is gone (the remove had succeeded)");
  check(files.has(method::INDEX_TMP_PATH) && files.files[method::INDEX_TMP_PATH] == changed.serialise(),
        "the temp file is kept: it is the only complete copy");
  files.failRenameDest.clear();
  files.put(bookPath("Z 99999999"), BOOK_BYTES);
  Library recovered;
  const auto recoveredReport = recovered.load(files);
  check(recoveredReport.recovered, "the next load recovers from the temp file");
  check(recovered.find(bookPath("Z 99999999")) != nullptr, "with what the failed save was writing");
  check(files.has(method::INDEX_PATH) && !files.has(method::INDEX_TMP_PATH), "and puts the index back");

  files.failEnsure = true;
  check(!changed.save(files), "no folder, no save");
}

void testCommitDeletesAfterTheIndex() {
  fake::FakeFiles files;
  Library library;
  files.put(bookPath("A 11111111"), BOOK_BYTES);
  library.add(entry("A", "A 11111111", 100));
  library.save(files);

  // New version of A: the old file goes only once the new index is written.
  files.put(bookPath("A 22222222"), BOOK_BYTES);
  files.log.clear();
  const auto garbage = library.add(entry("A", "A 22222222", 200, SHA_B));
  check(library.commit(files, garbage), "commit succeeds");
  const size_t replace = files.indexOfLog("replace /Lamina/Method/index.json.tmp /Lamina/Method/index.json");
  const size_t del = files.indexOfLog("removeBook " + bookPath("A 11111111"));
  check(replace != SIZE_MAX && del != SIZE_MAX && replace < del, "the old version is deleted after the index write");
  check(!files.has(bookPath("A 11111111")) && files.has(bookPath("A 22222222")), "old file gone, new file kept");

  // If the index cannot be written, nothing is deleted.
  files.put(bookPath("A 33333333"), BOOK_BYTES);
  files.failReplaceDest.insert(method::INDEX_PATH);
  files.log.clear();
  const auto garbage2 = library.add(entry("A", "A 33333333", 300, SHA_C));
  check(!library.commit(files, garbage2), "commit reports the failed write");
  check(files.has(bookPath("A 22222222")), "the old version is kept when the index write failed");
  checkEq(files.count("removeBook " + bookPath("A 22222222")), 0, "no delete was attempted");

  // A path that is still indexed is never deleted, even if a caller lists it.
  files.failReplaceDest.clear();
  check(library.commit(files, {bookPath("A 33333333"), "/etc/passwd", "/Lamina/Method/../x.xtc"}), "commit with odd garbage");
  check(files.has(bookPath("A 33333333")), "an indexed book is never garbage");
  checkEq(files.count("removeBook /etc/passwd"), 0, "paths outside the Method folder are never deleted");
}

void testLoadFreshAndEmpty() {
  fake::FakeFiles files;
  Library library;
  const auto report = library.load(files);
  check(!report.indexRead && !report.saved && report.dropped == 0 && report.adopted == 0, "nothing on the card");
  check(library.entries().empty(), "an empty shelf");
  check(files.log.size() == 2 && files.count("remove /Lamina/Method/.incoming.xtc") == 1 &&
            files.count("remove /Lamina/Method/index.json.tmp") == 1,
        "an empty card only gets the stale-file cleanup, no writes");
  check(!files.has(method::INDEX_PATH), "no index is written for an empty shelf");
}

void testLoadReconcile() {
  fake::FakeFiles files;
  Library seed;
  seed.add(entry("Kept", "Kept 11111111", 300, SHA_A, true));
  seed.add(entry("Gone", "Gone 22222222", 200, SHA_B));
  files.put(bookPath("Kept 11111111"), BOOK_BYTES);  // Gone has no file
  files.progress[bookPath("Gone 22222222")] = 2;     // stale progress for a missing book
  seed.save(files);

  // A stale partial download and an unindexed book.
  files.put(method::INCOMING_PATH, "partial");
  files.put(bookPath("Found 33333333"), BOOK_BYTES, 250);
  files.bookTitles[bookPath("Found 33333333")] = "Found in XTC metadata";
  files.bookPages[bookPath("Found 33333333")] = 12;
  files.put("/Lamina/Method/.hidden.xtc", BOOK_BYTES, 260);       // dot file
  files.put("/Lamina/Method/Odd'name.xtc", BOOK_BYTES, 270);      // not a name the reader writes
  files.put("/Lamina/Method/notes.txt", "x", 280);                // not a book
  files.put("/Lamina/Method/Broken 44444444.xtc", "junk", 290);   // not an XTC
  files.log.clear();

  Library library;
  const auto report = library.load(files);
  check(report.indexRead, "index read");
  checkEq(report.dropped, size_t{1}, "the entry without a file is dropped");
  checkEq(report.adopted, size_t{1}, "one unindexed book is adopted");
  check(report.saved, "the index is rewritten after changes");
  checkEq(titles(library), (std::vector<std::string>{"Kept", "Found in XTC metadata"}), "kept and adopted, newest first");
  const Entry* found = library.find(bookPath("Found 33333333"));
  check(found != nullptr, "adopted entry exists");
  checkEq(found->pageCount, 12u, "page count from the parser");
  checkEq(found->receivedAt, int64_t{250}, "received time from the file time");
  check(found->sha256.empty() && found->bookId.empty(), "an adopted book has no hash or id");
  check(library.find(bookPath("Kept 11111111"))->opened, "opened flag kept");
  check(!files.has(method::INCOMING_PATH), "the stale partial download is removed");
  check(files.has(bookPath("Found 33333333")) && files.has("/Lamina/Method/.hidden.xtc") &&
            files.has("/Lamina/Method/Odd'name.xtc") && files.has("/Lamina/Method/Broken 44444444.xtc"),
        "reconcile never deletes a book it did not index");
  checkEq(files.count("removeCache " + bookPath("Gone 22222222")), 1, "the cache of a missing book is cleared");

  // The index on the card now matches, so a second load changes nothing.
  files.log.clear();
  Library again;
  const auto second = again.load(files);
  check(second.indexRead && !second.saved && second.dropped == 0 && second.adopted == 0, "a second load is a no-op");
  checkEq(again.serialise(), library.serialise(), "same library");
  checkEq(files.count("write /Lamina/Method/index.json.tmp"), 0, "and writes nothing");
}

void testAdoptionRules() {
  fake::FakeFiles files;
  // A book whose parser title is just the file name loses the hash suffix.
  files.put(bookPath("Plain title 1a2b3c4d"), BOOK_BYTES, 100);
  // Same title as an existing entry: not a second shelf row.
  Library seed;
  seed.add(entry("Existing", "Existing 11111111", 500));
  files.put(bookPath("Existing 11111111"), BOOK_BYTES);
  seed.save(files);
  files.put(bookPath("Existing 99999999"), BOOK_BYTES, 600);
  files.bookTitles[bookPath("Existing 99999999")] = "Existing";
  files.put(bookPath("Unparsable 55555555"), BOOK_BYTES, 700);
  files.unparsable.insert(bookPath("Unparsable 55555555"));

  Library library;
  const auto report = library.load(files);
  checkEq(report.adopted, size_t{1}, "only the plain book is adopted");
  check(library.find(bookPath("Plain title 1a2b3c4d")) != nullptr, "plain book adopted");
  checkEq(library.find(bookPath("Plain title 1a2b3c4d"))->title, std::string("Plain title"),
          "the file-name fallback title has no hash suffix");
  check(library.find(bookPath("Existing 99999999")) == nullptr, "a second book under an existing title is not adopted");
  check(files.has(bookPath("Existing 99999999")), "and it is not deleted");
  check(library.find(bookPath("Unparsable 55555555")) == nullptr, "an unreadable book is not adopted");

  // Capacity: the newest fit, the rest stay on the card untouched.
  fake::FakeFiles crowded;
  for (int i = 0; i < 15; ++i)
    crowded.put(bookPath("Book" + std::to_string(i) + " 11111111"), BOOK_BYTES, 1000 + i);
  Library shelf;
  const auto crowdedReport = shelf.load(crowded);
  checkEq(crowdedReport.adopted, method::MAX_METHODS, "twelve adopted");
  checkEq(shelf.entries().front().title, std::string("Book14"), "newest first");
  check(shelf.find(bookPath("Book2 11111111")) == nullptr && crowded.has(bookPath("Book2 11111111")),
        "the three oldest stay on the card but off the shelf");
  checkEq(crowded.count("removeBook " + bookPath("Book2 11111111")), 0, "adoption never deletes");
}

void testLoadOpenedFromProgress() {
  fake::FakeFiles files;
  Library seed;
  seed.add(entry("Read", "Read 11111111", 200));
  seed.add(entry("Unread", "Unread 22222222", 100));
  files.put(bookPath("Read 11111111"), BOOK_BYTES);
  files.put(bookPath("Unread 22222222"), BOOK_BYTES);
  seed.save(files);
  files.progress[bookPath("Read 11111111")] = 0;  // opened from Recents: the cover was drawn

  Library library;
  const auto report = library.load(files);
  checkEq(report.opened, size_t{1}, "one entry is marked opened from its progress file");
  check(library.find(bookPath("Read 11111111"))->opened, "the book with progress is opened");
  check(!library.find(bookPath("Unread 22222222"))->opened, "the other is still new");
  check(report.saved, "and the index records it");
}

void testLoadRecovery() {
  // Power cut between "remove old index" and "rename temp": only the complete temp file exists.
  fake::FakeFiles files;
  Library seed;
  seed.add(entry("Survivor", "Survivor 11111111", 100));
  files.put(bookPath("Survivor 11111111"), BOOK_BYTES);
  files.put(method::INDEX_TMP_PATH, seed.serialise());
  Library library;
  const auto report = library.load(files);
  check(report.recovered, "recovered from the temp file");
  checkEq(titles(library), (std::vector<std::string>{"Survivor"}), "the method came back");
  check(files.has(method::INDEX_PATH) && !files.has(method::INDEX_TMP_PATH), "index restored, temp gone");

  // A stale (possibly torn) temp file beside a good index is ignored and removed.
  fake::FakeFiles stale;
  stale.put(bookPath("Survivor 11111111"), BOOK_BYTES);
  seed.save(stale);
  stale.put(method::INDEX_TMP_PATH, "{\"version\":1,\"methods\":[{\"title\":");
  Library good;
  good.load(stale);
  checkEq(titles(good), (std::vector<std::string>{"Survivor"}), "the good index wins over a torn temp file");
  check(!stale.has(method::INDEX_TMP_PATH), "the stale temp file is removed");

  // A corrupt index is rebuilt from the books in the folder.
  fake::FakeFiles corrupt;
  corrupt.put(method::INDEX_PATH, "not json at all");
  corrupt.put(bookPath("Recovered 22222222"), BOOK_BYTES, 50);
  Library rebuilt;
  const auto rebuiltReport = rebuilt.load(corrupt);
  check(!rebuiltReport.indexRead && rebuiltReport.adopted == 1 && rebuiltReport.saved, "a corrupt index is rebuilt");
  check(rebuiltReport.indexDamaged && !rebuiltReport.indexUnreadable, "and reported as damaged");
  checkEq(titles(rebuilt), (std::vector<std::string>{"Recovered"}), "the book is back on the shelf");
  check(corrupt.files[method::INDEX_PATH] == rebuilt.serialise(), "and the index on the card is valid again");
  checkEq(corrupt.files[method::INDEX_BAD_PATH], std::string("not json at all"), "the damaged text is kept, not destroyed");

  // The damaged text cannot be set aside: it is not overwritten either, and nothing is written this session.
  fake::FakeFiles stuck;
  stuck.put(method::INDEX_PATH, "not json at all");
  stuck.put(bookPath("Recovered 22222222"), BOOK_BYTES, 50);
  stuck.failReplaceDest.insert(method::INDEX_BAD_PATH);
  Library held;
  const auto heldReport = held.load(stuck);
  check(heldReport.indexDamaged && !heldReport.saved, "no backup, no rewrite");
  checkEq(stuck.files[method::INDEX_PATH], std::string("not json at all"), "the damaged file is left exactly as it was");
  checkEq(titles(held), (std::vector<std::string>{"Recovered"}), "the shelf still shows what the folder holds");
  check(!held.save(stuck), "and nothing is written until the next load");
}

// An index that exists but cannot be read says nothing about its contents: the shelf is built in memory and the
// file on the card is never replaced by a rebuild (it held the progress marks, the kinds and the mailbox mark).
void testUnreadableIndexIsNeverOverwritten() {
  fake::FakeFiles files;
  Library seed;
  Entry known = entry("Known", "Known 11111111", 300);
  known.kind = method::Kind::Table;
  known.opened = true;
  seed.add(known);
  seed.advanceMark(41);
  files.put(bookPath("Known 11111111"), BOOK_BYTES);
  seed.save(files);
  files.put(bookPath("Stray 22222222"), BOOK_BYTES, 200);
  const std::string onCard = files.files[method::INDEX_PATH];

  files.failRead.insert(method::INDEX_PATH);
  Library library;
  const auto report = library.load(files);
  check(report.indexUnreadable && !report.indexRead && !report.indexDamaged, "reported as unreadable");
  check(!report.saved, "nothing was written");
  checkEq(files.files[method::INDEX_PATH], onCard, "the index on the card is exactly as it was");
  check(!files.has(method::INDEX_BAD_PATH), "and nothing was moved aside");
  check(report.adopted == 2, "the folder's books are shown in memory");
  check(!library.save(files), "save refuses");
  check(!library.commit(files, {}), "commit refuses");
  checkEq(files.files[method::INDEX_PATH], onCard, "still untouched");
  const auto garbage = library.add(entry("New", "New 33333333", 500));
  check(!library.commit(files, garbage), "filing a book fails rather than replace the index");
  checkEq(files.files[method::INDEX_PATH], onCard, "and the index is still the one on the card");

  // The next successful read gets everything back, including what only the index knew.
  files.failRead.clear();
  Library again;
  const auto next = again.load(files);
  check(next.indexRead && !next.indexUnreadable, "readable again");
  check(again.find(bookPath("Known 11111111")) && again.find(bookPath("Known 11111111"))->kind == method::Kind::Table &&
            again.mark() == 41,
        "the kind and the mailbox mark were never lost");
  check(again.save(files), "and saving works again");

  // Too large to read is the same thing.
  fake::FakeFiles big;
  big.put(method::INDEX_PATH, std::string(method::MAX_INDEX_BYTES + 1, ' '));
  Library huge;
  const auto bigReport = huge.load(big);
  check(bigReport.indexUnreadable && !bigReport.saved && big.files[method::INDEX_PATH].size() == method::MAX_INDEX_BYTES + 1,
        "an index over the size cap is left alone");
}

void testMigration() {
  const std::string legacyJson = "{\"book_id\":\"b7\",\"sha256\":\"" + SHA_B +
                                 "\",\"path\":\"/Lamina/Method/Old method 12345678.xtc\",\"title\":\"Old method\"}";
  {
    // The Stage-0 book is imported and the state file is deleted once the index holds it.
    fake::FakeFiles files;
    files.put(method::LEGACY_STATE_PATH, legacyJson);
    files.put(bookPath("Old method 12345678"), BOOK_BYTES, 777);
    files.bookPages[bookPath("Old method 12345678")] = 9;
    files.progress[bookPath("Old method 12345678")] = 3;
    Library library;
    const auto report = library.load(files);
    check(report.migrated && report.saved, "migrated and saved");
    checkEq(library.mark(), 7u, "the imported book's mailbox id is already dealt with");
    check(library.entries()[0].kind == method::Kind::Method, "the imported book is a method");
    check(!files.has(method::LEGACY_STATE_PATH), "the old state file is deleted");
    checkEq(library.entries().size(), size_t{1}, "one method");
    const Entry& e = library.entries()[0];
    checkEq(e.title, std::string("Old method"), "title imported");
    checkEq(e.sha256, SHA_B, "hash imported");
    checkEq(e.bookId, std::string("b7"), "book id imported");
    checkEq(e.pageCount, 9u, "page count from the parser");
    check(e.opened, "a book with progress is not new");
    // The index was written before the old file went.
    const size_t replace = files.indexOfLog("replace /Lamina/Method/index.json.tmp /Lamina/Method/index.json");
    const size_t del = files.indexOfLog("remove /.crosspoint/lamina-method.json");
    check(replace != SIZE_MAX && del != SIZE_MAX && replace < del, "the state file goes after the index is written");
    Library next;
    next.load(files);
    checkEq(next.entries().size(), size_t{1}, "the next load sees one method, not two");
  }
  {
    // If the index cannot be written, the state file stays so the import is retried.
    fake::FakeFiles files;
    files.put(method::LEGACY_STATE_PATH, legacyJson);
    files.put(bookPath("Old method 12345678"), BOOK_BYTES);
    files.failReplaceDest.insert(method::INDEX_PATH);
    Library library;
    const auto report = library.load(files);
    check(report.migrated && !report.saved, "import happened but the save failed");
    check(files.has(method::LEGACY_STATE_PATH), "the state file is kept until the index is on the card");
  }
  {
    // The old book is gone: nothing to import, and the state file is removed.
    fake::FakeFiles files;
    files.put(method::LEGACY_STATE_PATH, legacyJson);
    Library library;
    const auto report = library.load(files);
    check(!report.migrated && library.entries().empty(), "nothing to import");
    check(!files.has(method::LEGACY_STATE_PATH), "the dangling state file is removed");
  }
  {
    // A damaged or hostile state file is ignored and removed.
    fake::FakeFiles files;
    files.put(method::LEGACY_STATE_PATH,
              "{\"book_id\":\"b7\",\"sha256\":\"" + SHA_B + "\",\"path\":\"/sdcard/other.xtc\",\"title\":\"x\"}");
    files.put("/sdcard/other.xtc", BOOK_BYTES);
    Library library;
    library.load(files);
    check(library.entries().empty() && !files.has(method::LEGACY_STATE_PATH), "a path outside the folder is not imported");
    check(files.has("/sdcard/other.xtc"), "and the file it named is untouched");
  }
  {
    // The book is already indexed (migration ran before but the state file survived): no duplicate.
    fake::FakeFiles files;
    Library seed;
    seed.add(entry("Old method", "Old method 12345678", 100, SHA_B));
    files.put(bookPath("Old method 12345678"), BOOK_BYTES);
    seed.save(files);
    files.put(method::LEGACY_STATE_PATH, legacyJson);
    Library library;
    library.load(files);
    checkEq(library.entries().size(), size_t{1}, "no duplicate entry");
    check(!files.has(method::LEGACY_STATE_PATH), "the state file is removed");
  }

  method::LegacyBook book;
  check(method::parseLegacyState(legacyJson, book), "legacy state parses");
  checkEq(book.title, std::string("Old method"), "legacy title");
  check(!method::parseLegacyState("{\"book_id\":\"x\"}", book), "incomplete legacy state is rejected");
  check(!method::parseLegacyState("", book), "empty legacy state is rejected");
  check(method::parseLegacyState("{\"book_id\":\"b1\",\"sha256\":\"" + SHA_A +
                                     "\",\"path\":\"/Lamina/Method/No title 1a2b3c4d.xtc\"}",
                                 book) &&
            book.title == "No title",
        "a legacy state without a title uses the file name");
}

void testMarkOpened() {
  // markOpened is sticky and reported once.
  Library library;
  library.add(entry("A", "A 11111111", 100));
  check(library.markOpened(bookPath("A 11111111")), "first open changes the entry");
  check(!library.markOpened(bookPath("A 11111111")), "second open changes nothing");
  check(!library.markOpened("/Lamina/Method/none.xtc"), "unknown path changes nothing");
}

method::Timer timer(unsigned page, unsigned seconds, const std::string& label = "", unsigned x = 32, unsigned y = 610,
                    unsigned w = 416, unsigned h = 96) {
  method::Timer t;
  t.page = static_cast<uint16_t>(page);
  t.seconds = seconds;
  t.label = label;
  t.x = static_cast<uint16_t>(x);
  t.y = static_cast<uint16_t>(y);
  t.w = static_cast<uint16_t>(w);
  t.h = static_cast<uint16_t>(h);
  return t;
}

// Timers ride in the index entry: written after "opened", read back by the same rules the mailbox's list is read by.
void testTimersInTheIndex() {
  Library library;
  Entry withTimers = entry("RNA extraction", "Extract 22222222", 200, SHA_B);
  withTimers.timers = {timer(4, 300, "Incubate"), timer(6, 3600, ""), timer(7, 45, "Spin \"hard\"", 0, 0, 24, 24)};
  Entry without = entry("Plasmid prep", "Plasmid 11111111", 100, SHA_C);
  library.add(withTimers);
  library.add(without);
  const std::string text = library.serialise();
  check(text.find(
            "\"opened\":false,\"timers\":[{\"page\":4,\"seconds\":300,\"label\":\"Incubate\",\"box\":[32,610,416,96]},"
            "{\"page\":6,\"seconds\":3600,\"box\":[32,610,416,96]},"
            "{\"page\":7,\"seconds\":45,\"label\":\"Spin \\\"hard\\\"\",\"box\":[0,0,24,24]}]}") != std::string::npos,
        "timers are written after opened, in the Worker's shape, escaped, with no label key when there is no label");
  check(text.find("Plasmid 11111111") != std::string::npos &&
            text.substr(text.find("Plasmid 11111111")).find("timers") == std::string::npos,
        "a book without timers writes no timers key");

  Library other;
  check(other.parse(text), "the index with timers parses");
  const Entry* back = other.find(bookPath("Extract 22222222"));
  check(back && back->timers == withTimers.timers, "timers come back exactly, in order");
  check(other.find(bookPath("Plasmid 11111111"))->timers.empty(), "and a book without timers has none");
  checkEq(other.serialise(), text, "the round trip is byte for byte");

  // Anything that fails validation drops that book's timers, not the book.
  const auto timersOf = [&](const std::string& timersJson, const std::string& sha = SHA_A,
                            const std::string& pages = "8") {
    std::string base = item("T", "/Lamina/Method/T aaaaaaaa.xtc", sha, pages);
    base.insert(base.size() - 1, ",\"timers\":" + timersJson);
    Library l;
    check(l.parse(json(base)), "an entry with a timers member parses");
    check(l.entries().size() == 1, "and the entry is kept");
    return l.entries()[0].timers;
  };
  check(timersOf("[{\"page\":3,\"seconds\":60,\"box\":[0,0,24,24]}]").size() == 1, "a valid set is kept");
  check(timersOf("[{\"page\":9,\"seconds\":60,\"box\":[0,0,24,24]}]").empty(),
        "a page past the entry's page_count drops the set");
  check(timersOf("[{\"page\":3,\"seconds\":60,\"box\":[0,0,24,24]},{\"page\":3,\"seconds\":61,\"box\":[0,0,24,24]}]")
            .empty(),
        "two timers on one page drop the set");
  check(timersOf("[{\"page\":3,\"seconds\":60,\"box\":[0,0,24,24]},{\"page\":4,\"seconds\":0,\"box\":[0,0,24,24]}]")
            .empty(),
        "one bad timer drops them all");
  check(timersOf("[{\"page\":3,\"seconds\":60,\"box\":[0,0,24,24],\"z\":1}]").empty(), "an unknown key drops the set");
  check(timersOf("{\"page\":3}").empty() && timersOf("null").empty() && timersOf("5").empty(),
        "timers that are not an array");
  check(timersOf("[{\"page\":3,\"seconds\":60,\"box\":[0,0,24,24]}]", "").empty(),
        "a book found on the card (no SHA-256) carries no timers");
  check(timersOf("[{\"page\":1,\"seconds\":60,\"box\":[0,0,24,24]}]", SHA_A, "1").size() == 1,
        "page 1 of a one-page book");

  // Past MAX_METHODS entries only the first twelve (in file order, newest first) keep their timers.
  std::string many;
  for (size_t i = 0; i < method::MAX_METHODS + 4; ++i) {
    const std::string base = item("M" + std::to_string(i), "/Lamina/Method/M" + std::to_string(i) + " aaaaaaaa.xtc",
                                  SHA_A, "5", std::to_string(1000 - i));
    many += (i ? "," : "") + base.substr(0, base.size() - 1) +
            ",\"timers\":[{\"page\":2,\"seconds\":9,\"box\":[0,0,24,24]}]}";
  }
  Library crowded;
  check(crowded.parse(json(many)), "sixteen entries with timers parse");
  size_t carrying = 0;
  for (const Entry& e : crowded.entries()) carrying += !e.timers.empty();
  checkEq(carrying, method::MAX_METHODS, "no more than twelve entries keep timers, so the index stays under its cap");

  // Timers survive being filed alongside other changes: a new version replaces the old entry, timers and all.
  Library lib2;
  Entry v1 = entry("Seeding", "Seeding 11111111", 100, SHA_A);
  v1.timers = {timer(2, 60)};
  lib2.add(v1);
  Entry v2 = entry("Seeding", "Seeding 22222222", 200, SHA_B);
  lib2.add(v2);
  check(lib2.entries().size() == 1 && lib2.entries()[0].timers.empty(), "a new version brings its own timers (none)");
  // Entries from the shelf keep their timers through save and load on the card.
  fake::FakeFiles files;
  Library saved;
  Entry filed = entry("Extract", "Extract 33333333", 300, SHA_C);
  filed.timers = {timer(5, 900, "Lyse")};
  saved.add(filed);
  files.put(filed.path, BOOK_BYTES);
  check(saved.save(files), "saved");
  Library loaded;
  loaded.load(files);
  check(loaded.find(filed.path) && loaded.find(filed.path)->timers == filed.timers, "timers survive save and load");

  // A book found on the card (no SHA-256) is never written with timers, and neither is a set larger than the Worker
  // allows.
  Library handmade;
  Entry copied = entry("Copied", "Copied aaaaaaaa", 100, "");
  copied.bookId = "";
  copied.timers = {timer(3, 60)};
  handmade.add(copied);
  check(handmade.serialise().find("timers") == std::string::npos,
        "a hand-copied book carries no timers into the index");
  Library oversize;
  Entry big = entry("Big", "Big 11111111", 100, SHA_B);
  for (unsigned page = 1; page <= 8; ++page) big.timers.push_back(timer(page, 60, std::string(40, 'x')));
  std::string bigText;
  method::appendTimersJson(bigText, big.timers);
  check(bigText.size() < method::MAX_TIMERS_JSON_BYTES, "precondition: eight timers are within the limit");
  oversize.add(big);
  check(oversize.serialise().find("timers") != std::string::npos, "a set within the limit is written");
  for (unsigned page = 9; page <= 60; ++page) big.timers.push_back(timer(page, 60, std::string(40, 'x')));
  Library over2;
  big.pageCount = 100;
  over2.add(big);
  check(over2.serialise().find("timers") == std::string::npos,
        "a set past 4096 bytes is left out rather than written where it would not read back");
  Library readBack;
  check(readBack.parse(over2.serialise()) && readBack.entries().size() == 1 && readBack.entries()[0].timers.empty(),
        "and the book itself is still in the index");
  // markOpened and an unrelated rewrite do not touch them.
  loaded.markOpened(filed.path);
  Library again;
  check(again.parse(loaded.serialise()) && again.find(filed.path)->timers == filed.timers &&
            again.find(filed.path)->opened,
        "and survive the opened flag");
}

// A parse that runs out of heap says nothing about the file: it must not be treated as a damaged index.
// (The host cannot run out of memory, so this only checks that an ordinary failure is still a failure.)
void testDamagedIndexStillDamaged() {
  fake::FakeFiles files;
  files.put(method::INDEX_PATH, "{\"version\":1,\"methods\":[");
  Library library;
  const auto report = library.load(files);
  check(report.indexDamaged && !report.indexUnreadable && !library.readOnly(),
        "a torn index is still damaged, not unreadable");
}

// The reader finds one book in index.json without loading the file: fed in pieces of any size.
void testIndexScanner() {
  Library library;
  for (int i = 0; i < 6; ++i) {
    Entry e = entry("Book " + std::to_string(i), "Book " + std::to_string(i) + " 1111111" + std::to_string(i), 100 + i,
                    std::string(64, static_cast<char>('a' + i)));
    e.kind = i == 2 ? method::Kind::Table : method::Kind::Method;
    e.pageCount = 10;
    if (i % 2 == 0) e.timers = {timer(3, 300 + i, "Step \"" + std::to_string(i) + "\""), timer(5, 60)};
    library.add(e);
  }
  Entry handCopied = entry("Copied", "Copied aaaaaaaa", 50, "");
  handCopied.bookId = "";
  library.add(handCopied);
  const std::string text = library.serialise();

  const auto scan = [&](const std::string& path, size_t piece) {
    method::IndexScanner scanner(path);
    for (size_t at = 0; at < text.size() && !scanner.found(); at += piece)
      scanner.feed(std::string_view(text).substr(at, piece));
    scanner.finish();
    return scanner;
  };
  for (const size_t piece : {size_t{1}, size_t{2}, size_t{7}, size_t{64}, size_t{511}, size_t{512}, size_t{100000}}) {
    for (const Entry& want : library.entries()) {
      const method::IndexScanner scanner = scan(want.path, piece);
      check(scanner.found(), "every book is found, whatever the piece size");
      const Entry& got = scanner.entry();
      check(got.path == want.path && got.title == want.title && got.kind == want.kind &&
                got.pageCount == want.pageCount && got.timers == want.timers && got.sha256 == want.sha256,
            "and read the way the library reads it, timers included");
    }
    check(!scan(bookPath("Not there 12345678"), piece).found(), "a book the index does not list is not found");
  }
  // A last entry with no newline after it is still found when the input ends.
  const std::string noTrailing =
      "{\"version\":1,\"mark\":0,\"methods\":[\n" + item("Only", "/Lamina/Method/Only aaaaaaaa.xtc");
  method::IndexScanner tail("/Lamina/Method/Only aaaaaaaa.xtc");
  check(!tail.feed(noTrailing) && tail.finish() && tail.entry().title == "Only",
        "the last line is read at the end of the input");
  // Paths the scanner will not look for, a line that is too long, and a title that imitates the path key.
  check(!method::IndexScanner("").feed(text) && !method::IndexScanner("/Lamina/Method/../x.xtc").feed(text) &&
            !method::IndexScanner("/etc/passwd").feed(text),
        "a path outside the library is never searched for");
  method::IndexScanner longLine(bookPath("Long aaaaaaaa"));
  longLine.feed(std::string(100000, 'x') + "\"path\":\"" + bookPath("Long aaaaaaaa") + "\"\n");
  check(!longLine.found() && !longLine.finish(), "a line longer than an entry can be is skipped");
  Library tricky;
  Entry decoy = entry("x\"path\":\"/Lamina/Method/Real 11111111.xtc\"y", "Decoy 22222222", 100, SHA_B);
  tricky.add(decoy);
  const std::string decoyText = tricky.serialise();
  method::IndexScanner real("/Lamina/Method/Real 11111111.xtc");
  real.feed(decoyText);
  check(!real.finish(), "a title that spells out another book's path key does not match it (quotes are escaped)");

  // A line that names the path inside another member is not that book's line (the entry's own path decides).
  {
    const std::string wanted = bookPath("Wanted aaaaaaaa");
    const std::string other = bookPath("Other bbbbbbbb");
    const std::string tricky = "{\"version\":1,\"mark\":0,\"methods\":[\n{\"title\":\"Other\",\"path\":\"" + other +
                               "\",\"sha256\":\"" + SHA_A + "\",\"page_count\":4,\"note\":{\"path\":\"" + wanted +
                               "\"}}\n]}\n";
    method::IndexScanner scanner(wanted);
    scanner.feed(tricky);
    check(!scanner.finish(), "a line whose own path is another book's is not this book's");
  }
  // A line longer than any entry this reader writes is skipped, even when it would parse.
  {
    const std::string wanted = bookPath("Padded aaaaaaaa");
    const std::string padded = "{\"version\":1,\"mark\":0,\"methods\":[\n{\"title\":\"Padded\",\"path\":\"" + wanted +
                               "\",\"page_count\":4,\"pad\":\"" + std::string(9000, 'p') + "\"}\n]}\n";
    method::IndexScanner scanner(wanted);
    scanner.feed(padded);
    check(!scanner.finish(), "an entry line past MAX_ENTRY_BYTES + MAX_ENTRY_TIMER_BYTES is not read");
    const std::string fits = "{\"version\":1,\"mark\":0,\"methods\":[\n{\"title\":\"Padded\",\"path\":\"" + wanted +
                             "\",\"page_count\":4,\"pad\":\"" + std::string(3000, 'p') + "\"}\n]}\n";
    method::IndexScanner ok(wanted);
    ok.feed(fits);
    check(ok.finish() && ok.entry().title == "Padded", "while a long one that still fits is");
  }
  // A line that names the path but is not an entry is not a match either.
  method::IndexScanner junk(bookPath("Junk aaaaaaaa"));
  junk.feed("\"path\":\"" + bookPath("Junk aaaaaaaa") + "\" is not json\n");
  check(!junk.found(), "a line that names the path but does not parse");
}
}  // namespace

void testUpdateTimers() {
  Library library;
  Entry held = entry("Protocol", "Protocol 11111111", 100, SHA_A);
  library.add(held);
  Entry copied = entry("Copied", "Copied aaaaaaaa", 90, "");
  copied.bookId = "";
  library.add(copied);
  library.add(entry("Later", "Later 33333333", 80, SHA_B));
  const std::vector<method::Timer> set = {timer(3, 300, "Incubate")};
  check(library.updateTimers(bookPath("Protocol 11111111"), SHA_A, set), "the held entry takes a new set");
  check(library.find(bookPath("Protocol 11111111"))->timers == set, "and has it");
  check(!library.updateTimers(bookPath("Protocol 11111111"), SHA_A, set), "the same set changes nothing");
  check(library.updateTimers(bookPath("Protocol 11111111"), SHA_A, {}) &&
            library.find(bookPath("Protocol 11111111"))->timers.empty(),
        "an empty set takes them away");
  check(library.updateTimers("/Lamina/Method/Other name 22222222.xtc", SHA_A, set),
        "found by SHA-256 when the file name differs");
  check(!library.updateTimers(bookPath("Copied aaaaaaaa"), "", set) &&
            library.find(bookPath("Copied aaaaaaaa"))->timers.empty(),
        "a book found on the card never takes timers");
  check(!library.updateTimers(bookPath("Nothing aaaaaaaa"), SHA_C, set), "an unknown book is not found");
  const std::vector<method::Timer> other = {timer(2, 60, "Spin")};
  check(library.updateTimers(bookPath("Protocol 11111111"), "", other) &&
            library.find(bookPath("Protocol 11111111"))->timers == other,
        "found by its file name when no SHA-256 is given");
  check(library.updateTimers(bookPath("Later 33333333"), "", set) &&
            library.find(bookPath("Later 33333333"))->timers == set,
        "also past an entry that has no SHA-256");
  check(!library.updateTimers("/Lamina/Method/Other name 22222222.xtc", "", set),
        "and an empty SHA-256 matches nothing by itself");
}

// The chapter menu asks the index what a book is: "Steps" for a method, "Pages" for reference.
void testIndexedKind() {
  Library library;
  Entry procedure = entry("Example column clean-up", "Example 11111111", 300);
  Entry table = entry("Seeding density", "Seeding 22222222", 200);
  table.kind = method::Kind::Table;
  Entry plate = entry("Plate 3", "Plate 33333333", 100);
  plate.kind = method::Kind::Plate;
  Entry board = entry("Bench board", "Board 44444444", 50);
  board.kind = method::Kind::Board;
  for (const Entry& e : {procedure, table, plate, board}) library.add(e);
  const std::string text = library.serialise();
  using method::Kind;
  using method::indexedKind;
  check(indexedKind(text, bookPath("Example 11111111")) == Kind::Method, "a method is a method");
  check(indexedKind(text, bookPath("Seeding 22222222")) == Kind::Table, "a table is a table");
  check(indexedKind(text, bookPath("Plate 33333333")) == Kind::Plate, "a plate map is a plate map");
  check(indexedKind(text, bookPath("Board 44444444")) == Kind::Board, "a board is a board");
  // Only a method has steps: the others are read as pages.
  check(method::kindHasSteps(Kind::Method) && !method::kindHasSteps(Kind::Table) &&
            !method::kindHasSteps(Kind::Plate) && !method::kindHasSteps(Kind::Board),
        "only a method has steps");
  // Unknown is a method: a book the index does not list, an index that is missing or damaged, an unknown kind.
  check(indexedKind(text, bookPath("Not indexed aaaaaaaa")) == Kind::Method, "a book the index does not list");
  check(indexedKind("", bookPath("Seeding 22222222")) == Kind::Method, "no index text");
  check(indexedKind("{", bookPath("Seeding 22222222")) == Kind::Method, "a damaged index");
  check(indexedKind("[]", bookPath("Seeding 22222222")) == Kind::Method, "not an index");
  check(indexedKind(text, "") == Kind::Method && indexedKind(text, "/Books/Seeding 22222222.xtc") == Kind::Method,
        "a path outside the library");
  const std::string unknown = "{\"title\":\"A\",\"path\":\"/Lamina/Method/A aaaaaaaa.xtc\",\"page_count\":3,\"kind\":\"sheet\"}";
  check(indexedKind(json(unknown), "/Lamina/Method/A aaaaaaaa.xtc") == Kind::Method, "an unknown kind reads as a method");
  const std::string older = "{\"title\":\"B\",\"path\":\"/Lamina/Method/B bbbbbbbb.xtc\",\"page_count\":3}";
  check(indexedKind(json(older), "/Lamina/Method/B bbbbbbbb.xtc") == Kind::Method, "an index from before kinds");
}

int main() {
  testSerialiseFormat();
  testParseValidation();
  testAddReplaceAndPrune();
  testRemove();
  testRemoveAndCommit();
  testMarkAndKind();
  testHasVersion();
  testAtomicSave();
  testCommitDeletesAfterTheIndex();
  testLoadFreshAndEmpty();
  testLoadReconcile();
  testAdoptionRules();
  testLoadOpenedFromProgress();
  testLoadRecovery();
  testUnreadableIndexIsNeverOverwritten();
  testEmptyIndexIsDamagedNotUnreadable();
  testTempFileRecovery();
  testWorstCaseIndexFitsTheReadCap();
  testReadOnlyFollowsTheLoad();
  testMigration();
  testMarkOpened();
  testIndexedKind();
  testTimersInTheIndex();
  testDamagedIndexStillDamaged();
  testIndexScanner();
  testUpdateTimers();
  std::cout << "method library: " << fake::checkCount() << " checks passed\n";
  return 0;
}
