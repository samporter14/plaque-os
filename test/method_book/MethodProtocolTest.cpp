#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "MethodProtocol.h"
#include "MethodRoute.h"

namespace {
int checks = 0;
void check(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

const std::string DEVICE = "sams-reader";
const std::string HASH(64, 'a');

std::string reply(const std::string& id = "b12", const std::string& title = "PCR setup, 50 \\u00b5L",
                  const std::string& hash = HASH, const std::string& size = "123456", const std::string& pages = "9",
                  const std::string& url = "") {
  const std::string path = url.empty() ? "/api/device/" + DEVICE + "/book/" + id : url;
  return "{\"book_id\":\"" + id + "\",\"title\":\"" + title + "\",\"sha256\":\"" + hash + "\",\"byte_length\":" + size +
         ",\"page_count\":" + pages + ",\"created_at\":1770000000,\"book_url\":\"" + path + "\"}";
}

bool parses(const std::string& json, method::BookMetadata& out) { return method::parseBook(json, DEVICE, out); }
std::string name(const std::string& title, const std::string& hash = HASH) {
  char out[96];
  return method::bookFileName(title, hash, out, sizeof(out)) ? std::string(out) : std::string("<fail>");
}
}  // namespace

int main() {
  method::BookMetadata book;
  check(parses(reply(), book) && !book.empty, "valid reply");
  check(std::string(book.bookId) == "b12" && std::string(book.sha256) == HASH, "ids copied");
  check(book.byteLength == 123456 && book.pageCount == 9, "sizes copied");
  check(std::string(book.title) == "PCR setup, 50 \xC2\xB5L", "unicode title decoded");
  check(std::string(book.bookUrl) == "/api/device/sams-reader/book/b12", "url copied");

  check(parses("{\"book_id\":null}", book) && book.empty, "no book");
  check(!parses("{}", book), "missing book_id rejected");
  check(!parses("[]", book), "array rejected");
  check(!parses("{", book), "malformed rejected");
  check(!parses("", book), "empty body rejected");
  check(!parses(std::string(2049, ' '), book), "oversized metadata rejected");
  check(!method::parseBook(reply(), "bad id", book), "bad device id rejected");

  // The book_url is appended to the configured origin with the Bearer token attached.
  check(!parses(reply("b12", "T", HASH, "10", "1", "@evil.example/x"), book), "authority injection rejected");
  check(!parses(reply("b12", "T", HASH, "10", "1", "https://evil.example/api/device/sams-reader/book/b12"), book),
        "absolute url rejected");
  check(!parses(reply("b12", "T", HASH, "10", "1", "/api/device/other/book/b12"), book), "other device rejected");
  check(!parses(reply("b12", "T", HASH, "10", "1", "/api/device/sams-reader/book/b13"), book), "other book rejected");
  check(!parses(reply("b12", "T", HASH, "10", "1", "/api/device/sams-reader/book/b12?x=1"), book), "query rejected");
  check(!parses(reply("b12", "T", HASH, "10", "1", "/api/device/sams-reader/book/b12/"), book),
        "trailing slash rejected");

  for (const auto* id : {"12", "b", "B12", "b1x", "b12345678901234567890", "../b1", "b1\\u0000"})
    check(!parses(reply(id, "T", HASH, "10", "1", "/api/device/sams-reader/book/b12"), book), "bad book id rejected");

  check(!parses(reply("b12", "T", std::string(63, 'a')), book), "short hash rejected");
  check(!parses(reply("b12", "T", std::string(64, 'A')), book), "uppercase hash rejected");
  check(!parses(reply("b12", "T", std::string(64, 'g')), book), "non-hex hash rejected");
  for (const auto* size : {"0", "-1", "1900001", "4294967296", "\"123\"", "1.5", "null"})
    check(!parses(reply("b12", "T", HASH, size), book), "bad byte_length rejected");
  check(parses(reply("b12", "T", HASH, "1900000"), book), "size cap accepted");
  check(parses(reply("b12", "T", HASH, "1"), book), "minimum size accepted");
  for (const auto* pages : {"0", "201", "-3", "\"9\"", "null"})
    check(!parses(reply("b12", "T", HASH, "10", pages), book), "bad page_count rejected");
  check(parses(reply("b12", "T", HASH, "10", "200"), book), "page cap accepted");

  check(!parses(reply("b12", ""), book), "empty title rejected");
  check(!parses(reply("b12", "x\\u0000y"), book), "embedded NUL title rejected");
  check(parses(reply("b12", "line\\nbreak\\tx"), book) && std::string(book.title) == "line break x",
        "control characters in a title become spaces");
  check(parses(reply("b12", std::string(120, 'x')), book), "120-character title accepted");
  check(!parses(reply("b12", std::string(121, 'x')), book), "121-character title rejected");
  std::string wide;
  for (int i = 0; i < 120; ++i) wide += "\\u00e9";  // 120 characters, 240 bytes
  check(parses(reply("b12", wide), book), "120 multibyte characters accepted");
  check(!parses("{\"book_id\":\"b1\"}", book), "missing fields rejected");

  // File names: ASCII letters, digits, space, '-' and '_' plus the first 8 hex digits.
  check(name("PCR setup", HASH) == "PCR setup aaaaaaaa.xtc", "plain title");
  check(name("  Western blot: day 2 / gels  ") == "Western blot day 2 gels aaaaaaaa.xtc", "punctuation collapses");
  check(name("a\xC2\xB5L assay") == "a L assay aaaaaaaa.xtc", "non-ASCII collapses");
  check(name("../../etc/passwd") == "etc passwd aaaaaaaa.xtc", "no path separators or dots");
  check(name("snake_case-and-dashes") == "snake_case-and-dashes aaaaaaaa.xtc", "underscore and dash kept");
  check(name("\xC2\xB5\xC2\xB5") == "Book aaaaaaaa.xtc", "nothing usable falls back");
  check(name("") == "Book aaaaaaaa.xtc", "empty falls back");
  check(name(std::string(200, 'x')) == std::string(40, 'x') + " aaaaaaaa.xtc", "length capped");
  check(name("abc", "0123456789abcdef" + std::string(48, '0')) == "abc 01234567.xtc", "first 8 hex digits");
  check(name("abc", std::string(64, 'Z')) == "<fail>", "bad hash refused");
  char tiny[8];
  check(!method::bookFileName("Protocol", HASH, tiny, sizeof(tiny)), "short buffer refused");
  char path[method::MAX_PATH_BYTES];
  check(method::bookPath("Gel run", HASH, path, sizeof(path)) &&
            std::string(path) == "/Lamina/Method/Gel run aaaaaaaa.xtc",
        "book path");
  check(method::ownedBookPath(path), "built path is owned");

  // Only plain files inside the Method folder may ever be deleted.
  for (const auto* ok :
       {"/Lamina/Method/Gel run aaaaaaaa.xtc", "/Lamina/Method/a.xtc", "/Lamina/Method/A-b_c 12345678.xtc"})
    check(method::ownedBookPath(ok), "owned path");
  for (const auto* bad :
       {"", "/", "/Lamina/Method/", "/Lamina/Method/.xtc", "/Lamina/Method/.incoming.xtc",
        "/Lamina/Method/sub/book.xtc", "/Lamina/Method/../Books/book.xtc", "/Lamina/Method/book.epub",
        "/Lamina/Method/book.xtc.bak", "/Lamina/Methods/book.xtc", "/Books/book.xtc", "Lamina/Method/book.xtc",
        "/Lamina/Method/..xtc", "/Lamina/Method/a\xC2\xB5.xtc", "/Lamina/Method/a\nb.xtc"})
    check(!method::ownedBookPath(bad), "foreign path rejected");
  check(!method::ownedBookPath(std::string("/Lamina/Method/") + std::string(100, 'a') + ".xtc"),
        "overlong path rejected");

  check(method::validSha256Hex(HASH) && !method::validSha256Hex(HASH + "a") && !method::validSha256Hex(""),
        "sha shape");
  uint8_t digest[32];
  for (int i = 0; i < 32; ++i) digest[i] = static_cast<uint8_t>(i * 8 + 1);
  char hex[method::SHA256_HEX_BYTES + 1];
  method::toHex(digest, hex);
  check(std::string(hex) == "0109111921293139414951596169717981899199a1a9b1b9c1c9d1d9e1e9f1f9", "hex digest");

  check(method::utf8Prefix("abcdef", 3) == 3 && method::utf8Prefix("abc", 10) == 3, "ascii prefix");
  check(method::utf8Prefix("a\xC2\xB5"
                           "b",
                           2) == 1,
        "never splits a sequence");
  check(method::utf8Prefix("\xE2\x82\xAC"
                           "x",
                           2) == 0,
        "three-byte sequence kept whole or dropped");
  // The two routes the Bearer token may be sent to, and nothing else. The single-book route /book is not one.
  for (const char* ok : {"/api/device/sams-reader/books", "/api/device/sams-reader/book/b12", "/api/device/sams-reader/book/b0"})
    check(method::routeAllowed(DEVICE, ok), "token route allowed");
  for (const char* bad :
       {"", "/", "/api/device/sams-reader", "/api/device/sams-reader/book", "/api/device/sams-reader/book/",
        "/api/device/sams-reader/book/12", "/api/device/sams-reader/book/b", "/api/device/sams-reader/book/b12/",
        "/api/device/sams-reader/book/b12?x=1", "/api/device/sams-reader/book/b12#x", "/api/device/sams-reader/books/",
        "/api/device/sams-reader/books?x=1", "/api/device/sams-reader/booksx", "/api/device/sams-reader/latest",
        "/api/device/other/books", "/api/device/sams-readerx/books", "api/device/sams-reader/books",
        "//evil.example/api/device/sams-reader/books", "/api/device/sams-reader/book/../books",
        "@evil.example/api/device/sams-reader/books", "/api/device/sams-reader/book/b123456789012345678901"})
    check(!method::routeAllowed(DEVICE, bad), "route refused");
  check(!method::routeAllowed("bad id", "/api/device/bad id/books"), "unusable device id refused");
  check(!method::routeAllowed("", "/api/device//books"), "empty device id refused");
  check(method::validBookId("b12") && method::validBookId("b0") && !method::validBookId("b") && !method::validBookId("12") &&
            !method::validBookId("b1x") && !method::validBookId(""),
        "book id shape");

  // Book ids as numbers (the high-water mark compares them).
  uint32_t number = 99;
  check(method::bookIdNumber("b12", number) && number == 12, "b12 is 12");
  check(method::bookIdNumber("b0", number) && number == 0, "b0 is 0");
  check(method::bookIdNumber("b4294967295", number) && number == 4294967295u, "the largest 32-bit id");
  check(!method::bookIdNumber("b4294967296", number), "an id past 32 bits is refused");
  check(!method::bookIdNumber("b123456789012", number) && !method::bookIdNumber("12", number) &&
            !method::bookIdNumber("", number) && !method::bookIdNumber("b1x", number),
        "malformed ids are refused");

  // Kinds.
  method::Kind kind = method::Kind::Board;
  check(method::parseKind("method", kind) && kind == method::Kind::Method, "kind method");
  check(method::parseKind("table", kind) && kind == method::Kind::Table, "kind table");
  check(method::parseKind("plate", kind) && kind == method::Kind::Plate, "kind plate");
  check(method::parseKind("board", kind) && kind == method::Kind::Board, "kind board");
  check(!method::parseKind("Method", kind) && !method::parseKind("", kind) && !method::parseKind("sheet", kind),
        "unknown kinds are refused");
  check(std::string(method::kindName(method::Kind::Plate)) == "plate", "kind name");

  // Every name the shelf can file is one the sleep and reader code recognise as a Method book.
  for (const std::string& title :
       {std::string("PCR setup, 50 \\u00b5L"), std::string("\xC2\xB5L \xE2\x82\xAC"), std::string(120, 'x'), std::string("a/b\\c:d*e?f"),
        std::string("..."), std::string(" . "), std::string("RNA extraction"), std::string("\x01\x02"), std::string("\xF0\x9F\xA7\xAA")}) {
    char out[96];
    check(method::bookPath(title, HASH, out, sizeof(out)) && method::ownedBookPath(out), "a filed book path is an owned path");
  }

  // GET /books.
  const auto entry = [](const std::string& id, const std::string& title, const std::string& kind = "method",
                        const std::string& hash = HASH, const std::string& url = "") {
    const std::string path = url.empty() ? "/api/device/" + DEVICE + "/book/" + id : url;
    return "{\"book_id\":\"" + id + "\",\"kind\":\"" + kind + "\",\"title\":\"" + title + "\",\"sha256\":\"" + hash +
           "\",\"byte_length\":1000,\"page_count\":4,\"created_at\":1770000000,\"book_url\":\"" + path + "\"}";
  };
  std::vector<method::BookMetadata> list;
  uint32_t newest = 0;
  size_t skipped = 0;
  const auto listed = [&](const std::string& json) { return method::parseBooks(json, DEVICE, list, newest, skipped); };
  check(listed("{\"books\":[]}") && list.empty() && newest == 0 && skipped == 0, "an empty mailbox");
  check(listed("{\"books\":[" + entry("b14", "Seeding sheet", "table") + "," + entry("b12", "Protocol") + "]}") &&
            list.size() == 2 && newest == 14 && skipped == 0,
        "two books");
  check(list[0].kind == method::Kind::Table && list[0].idNumber == 14 && std::string(list[0].bookId) == "b14" &&
            std::string(list[0].title) == "Seeding sheet" && list[0].byteLength == 1000 && list[0].pageCount == 4 &&
            std::string(list[0].bookUrl) == "/api/device/sams-reader/book/b14" && !list[0].empty,
        "entry fields read");
  check(list[1].kind == method::Kind::Method, "second entry is a method");
  // The entry without a kind is a method (what /book returned).
  check(listed("{\"books\":[{\"book_id\":\"b3\",\"title\":\"T\",\"sha256\":\"" + HASH +
               "\",\"byte_length\":10,\"page_count\":2,\"book_url\":\"/api/device/sams-reader/book/b3\"}]}") &&
            list.size() == 1 && list[0].kind == method::Kind::Method,
        "a missing kind reads as method");
  // A damaged entry is skipped on its own; the others survive and the count says so.
  const std::string mixed = "{\"books\":[" + entry("b20", "Good") + "," + entry("b19", "Bad kind", "sheet") + "," +
                            entry("b18", "Other device", "method", HASH, "/api/device/other/book/b18") + "," +
                            entry("b17", "Bad hash", "method", "xyz") + "," + entry("b16", "Query", "method", HASH,
                                                                                   "/api/device/sams-reader/book/b16?x=1") +
                            ",42,{}," + entry("x15", "Bad id") + "," + entry("b20", "Duplicate id") + "," +
                            entry("b13", "Also good") + "]}";
  check(listed(mixed) && list.size() == 2 && skipped == 8 && newest == 20, "damaged entries are skipped one by one");
  check(std::string(list[0].title) == "Good" && std::string(list[1].title) == "Also good", "the good ones survive in order");
  // An id past 32 bits cannot be compared with the mark: refused.
  check(listed("{\"books\":[" + entry("b99999999999", "Huge id") + "]}") && list.empty() && skipped == 1, "huge id refused");
  // More than twelve entries: only the first twelve are read.
  std::string many;
  for (int i = 0; i < 15; ++i) many += (i ? "," : "") + entry("b" + std::to_string(100 - i), "T" + std::to_string(i));
  check(listed("{\"books\":[" + many + "]}") && list.size() == method::MAX_LIST_BOOKS && newest == 100,
        "at most twelve entries");
  // The reply itself must be an object with a books array.
  check(!listed("") && !listed("{") && !listed("[]") && !listed("{}") && !listed("{\"books\":{}}") && !listed("{\"books\":null}"),
        "a reply that is not a list is refused");
  check(!method::parseBooks("{\"books\":[]}", "bad id", list, newest, skipped), "bad device id refused");
  check(!listed(std::string(method::MAX_LIST_BYTES + 1, ' ')), "oversized reply refused");
  // Twelve books with 120-character titles fit the cap.
  std::string longTitle(120, 'T');
  std::string full;
  for (int i = 0; i < 12; ++i) full += (i ? "," : "") + entry("b" + std::to_string(200 - i), longTitle);
  check(full.size() + 12 < method::MAX_LIST_BYTES && listed("{\"books\":[" + full + "]}") && list.size() == 12,
        "twelve long titles fit under 12 KB");

  // Back from a book: a Method book always returns to the shelf, whatever the press and the setting; every other
  // book keeps the stock rules (long press: file browser, short press: Home, swapped by the setting).
  for (const bool longPress : {false, true}) {
    for (const bool swapped : {false, true}) {
      check(!method::backOpensFileBrowser(true, longPress, swapped), "a Method book never opens the file browser on Back");
      check(method::backOpensFileBrowser(false, longPress, swapped) == (longPress != swapped), "other books keep the stock rule");
    }
  }
  check(method::backOpensFileBrowser(false, true, false) && !method::backOpensFileBrowser(false, false, false) &&
            !method::backOpensFileBrowser(false, true, true) && method::backOpensFileBrowser(false, false, true),
        "the four stock cases, spelled out");
  // The route is decided by the same predicate the shelf's own files satisfy.
  for (const char* title : {"Example column clean-up", "Seeding density", "RNA extraction"}) {
    char out[96];
    check(method::bookPath(title, HASH, out, sizeof(out)) && !method::backOpensFileBrowser(method::ownedBookPath(out), true, false),
          "a book the shelf filed returns to the shelf even on a long press");
  }
  check(method::backOpensFileBrowser(method::ownedBookPath("/Books/Lab Handbook.epub"), true, false) &&
            method::backOpensFileBrowser(method::ownedBookPath("/Lamina/Method/sub/book.xtc"), true, false) &&
            method::backOpensFileBrowser(method::ownedBookPath("/Lamina/Method/.incoming.xtc"), true, false),
        "a path the shelf does not own keeps the file browser");

  // The name the check looks up before each request, from the configured origin.
  check(method::urlHost("https://mail.example.org") == "mail.example.org", "host of a bare origin");
  check(method::urlHost("https://mail.example.org/") == "mail.example.org", "and with a slash");
  check(method::urlHost("https://mail.example.org:8787/api/x") == "mail.example.org", "the port is not part of the host");
  check(method::urlHost("http://192.168.1.20:8787") == "192.168.1.20", "an http origin by address");
  check(method::urlHost("https://user:pass@host.test/x") == "host.test", "credentials are not part of the host");
  check(method::urlHost("https://host.test?x=1") == "host.test" && method::urlHost("https://host.test#frag") == "host.test",
        "a query or fragment ends the host");
  check(method::urlHost("https://[2001:db8::1]:8787/x").empty(), "an IPv6 literal is not looked up");
  check(method::urlHost("ftp://host.test").empty() && method::urlHost("host.test").empty() && method::urlHost("").empty() &&
            method::urlHost("https://").empty() && method::urlHost("https:///x").empty(),
        "not an http(s) origin, or no host");
  check(method::isIpv4Literal("192.168.1.20") && method::isIpv4Literal("0.0.0.0") && method::isIpv4Literal("255.255.255.255"),
        "dotted quads");
  check(!method::isIpv4Literal("mail.example.org") && !method::isIpv4Literal("1.2.3") && !method::isIpv4Literal("1.2.3.4.5") &&
            !method::isIpv4Literal("256.1.1.1") && !method::isIpv4Literal("1.2.3.") && !method::isIpv4Literal(".1.2.3") &&
            !method::isIpv4Literal("1..3.4") && !method::isIpv4Literal("1.2.3.x") && !method::isIpv4Literal("1234.1.1.1") &&
            !method::isIpv4Literal(""),
        "anything else is a name to look up");

  // ---- Timers (SPEC-DEVICES section 3): /books entries carry them, validated as strictly as the Worker does ----
  {
    const std::string goodTimer = "{\"page\":3,\"seconds\":300,\"label\":\"Incubate\",\"box\":[32,610,416,96]}";
    const auto withTimers = [&](const std::string& timers, const std::string& pages = "4") {
      return "{\"books\":[{\"book_id\":\"b5\",\"kind\":\"method\",\"title\":\"T\",\"sha256\":\"" + HASH +
             "\",\"byte_length\":1000,\"page_count\":" + pages + ",\"book_url\":\"/api/device/sams-reader/book/b5\"" +
             (timers.empty() ? "" : ",\"timers\":" + timers) + "}]}";
    };
    const auto timersOf = [&](const std::string& timers, const std::string& pages = "4") {
      std::vector<method::BookMetadata> out;
      uint32_t newest = 0;
      size_t skipped = 0;
      check(method::parseBooks(withTimers(timers, pages), DEVICE, out, newest, skipped) && out.size() == 1 &&
                skipped == 0,
            "a book with timers parses");
      return out[0];
    };
    method::BookMetadata t = timersOf("[" + goodTimer + "]");
    check(t.timers.size() == 1 && !t.timersDropped && t.timers[0].page == 3 && t.timers[0].seconds == 300 &&
              t.timers[0].label == "Incubate" && t.timers[0].x == 32 && t.timers[0].y == 610 && t.timers[0].w == 416 &&
              t.timers[0].h == 96,
          "a valid timer is read field by field");
    check(method::timerOnPage(t.timers, 3) == &t.timers[0] && method::timerOnPage(t.timers, 2) == nullptr,
          "found by page");
    check(timersOf("").timers.empty() && !timersOf("").timersDropped, "no timers key: no timers, nothing dropped");
    check(timersOf("[]").timers.empty() && !timersOf("[]").timersDropped, "an empty array is valid and empty");
    t = timersOf(
        "[{\"page\":1,\"seconds\":1,\"box\":[0,0,24,24]},{\"page\":4,\"seconds\":359999,\"label\":\"\\u00b5L "
        "spin\",\"box\":[456,776,24,24]}]");
    check(t.timers.size() == 2 && t.timers[0].label.empty() && t.timers[1].seconds == 359999 &&
              t.timers[1].label == "\xC2\xB5L spin",
          "the limits themselves are valid: 1 s, 99:59:59, a missing label, the smallest box in each corner");

    // Anything wrong anywhere drops the book's timers, all of them, and keeps the book.
    const auto bad = [&](const std::string& timer, const char* why, const std::string& pages = "4") {
      const method::BookMetadata book = timersOf("[" + goodTimer + "," + timer + "]", pages);
      check(book.timers.empty() && book.timersDropped, why);
      check(std::string(book.title) == "T" && book.pageCount == static_cast<unsigned>(std::stoi(pages)),
            "and the book is kept");
    };
    bad("{\"page\":0,\"seconds\":5,\"box\":[0,0,24,24]}", "page 0");
    bad("{\"page\":5,\"seconds\":5,\"box\":[0,0,24,24]}", "a page past the book's page_count");
    bad("{\"page\":2,\"seconds\":0,\"box\":[0,0,24,24]}", "zero seconds");
    bad("{\"page\":2,\"seconds\":360000,\"box\":[0,0,24,24]}", "more than 99:59:59");
    bad("{\"page\":2,\"seconds\":-5,\"box\":[0,0,24,24]}", "negative seconds");
    bad("{\"page\":2,\"seconds\":5.5,\"box\":[0,0,24,24]}", "fractional seconds");
    bad("{\"page\":2,\"seconds\":\"5\",\"box\":[0,0,24,24]}", "seconds as text");
    bad("{\"page\":2.0,\"seconds\":5,\"box\":[0,0,24,24]}", "a page written as a fraction");
    bad("{\"page\":3,\"seconds\":5,\"box\":[0,0,24,24]}", "two timers on one page");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,23,24]}", "box narrower than 24");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,24,23]}", "box shorter than 24");
    bad("{\"page\":2,\"seconds\":5,\"box\":[457,0,24,24]}", "box past the right edge");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,777,24,24]}", "box past the bottom edge");
    bad("{\"page\":2,\"seconds\":5,\"box\":[-1,0,24,24]}", "box at a negative x");
    bad("{\"page\":2,\"seconds\":5,\"box\":[4294967295,0,24,24]}", "an x that wraps round when w is added");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,4294967295,24,24]}", "a y that wraps round when h is added");
    bad("{\"page\":2,\"seconds\":5,\"box\":[4294967280,0,4294967295,24]}", "an x and w that sum to a small number");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,4294967295,24]}", "a width of 2^32 - 1");
    bad("{\"page\":2,\"seconds\":5,\"box\":[65536,0,24,24]}", "an x past 16 bits");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,24]}", "box with three numbers");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,24,24,0]}", "box with five numbers");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,24.5,24]}", "box with a fraction");
    bad("{\"page\":2,\"seconds\":5,\"box\":\"0,0,24,24\"}", "box as text");
    bad("{\"page\":2,\"seconds\":5}", "box missing");
    bad("{\"seconds\":5,\"box\":[0,0,24,24]}", "page missing");
    bad("{\"page\":2,\"box\":[0,0,24,24]}", "seconds missing");
    bad("{\"page\":2,\"seconds\":5,\"label\":\"\",\"box\":[0,0,24,24]}", "an empty label");
    bad("{\"page\":2,\"seconds\":5,\"label\":null,\"box\":[0,0,24,24]}", "a null label");
    bad("{\"page\":2,\"seconds\":5,\"label\":7,\"box\":[0,0,24,24]}", "a numeric label");
    bad("{\"page\":2,\"seconds\":5,\"label\":\"" + std::string(41, 'x') + "\",\"box\":[0,0,24,24]}",
        "a 41-character label");
    bad("{\"page\":2,\"seconds\":5,\"label\":\"a\\u0000b\",\"box\":[0,0,24,24]}", "a NUL inside the label");
    bad("{\"page\":2,\"seconds\":5,\"extra\":1,\"box\":[0,0,24,24]}", "an unknown key");
    bad("{\"page\":2,\"seconds\":5,\"box\":[0,0,24,24],\"v\":1}", "an unknown key after the box");
    bad("7", "a timer that is not an object");
    bad("[]", "a timer that is an array");

    // 40 characters is fine, whatever their width in bytes.
    t = timersOf("[{\"page\":2,\"seconds\":5,\"label\":\"" + std::string(40, 'x') + "\",\"box\":[0,0,24,24]}]");
    check(t.timers.size() == 1 && t.timers[0].label.size() == 40, "a 40-character label");
    std::string wide;
    for (int i = 0; i < 40; ++i) wide += "\\u00b5";
    t = timersOf("[{\"page\":2,\"seconds\":5,\"label\":\"" + wide + "\",\"box\":[0,0,24,24]}]");
    check(t.timers.size() == 1 && t.timers[0].label.size() == 80, "40 two-byte characters");
    // The Worker refuses control characters, invisible format characters and line separators, so does the reader; text
    // that is not well-formed UTF-8 is refused too.
    for (const char* unprintable :
         {"a\\tb", "a\\nb", "a\\u0001b", "a\\u007fb", "a\\u0085b", "a\\u00adb", "a\\u200bb", "a\\u200eb", "a\\u202eb",
          "a\\u2028b", "a\\u2029b", "a\\u2060b", "a\\ufeffb", "a\\ufff9b"})
      bad("{\"page\":2,\"seconds\":5,\"label\":\"" + std::string(unprintable) + "\",\"box\":[0,0,24,24]}", unprintable);
    {
      // The same rule as the Worker's, to its edges: it refuses the Unicode categories Cc, Cf, Zl and Zp (the ranges
      // below are what Node 25, Unicode 17, lists). Both ends of every range are refused here; the code point on either
      // side of a range is accepted unless it is in another range.
      struct Span {
        uint32_t first, last;
      };
      constexpr Span REFUSED[] = {{0x0, 0x1F},        {0x7F, 0x9F},       {0xAD, 0xAD},       {0x600, 0x605},
                                  {0x61C, 0x61C},     {0x6DD, 0x6DD},     {0x70F, 0x70F},     {0x890, 0x891},
                                  {0x8E2, 0x8E2},     {0x180E, 0x180E},   {0x200B, 0x200F},   {0x2028, 0x2029},
                                  {0x202A, 0x202E},   {0x2060, 0x2064},   {0x2066, 0x206F},   {0xFEFF, 0xFEFF},
                                  {0xFFF9, 0xFFFB},   {0x110BD, 0x110BD}, {0x110CD, 0x110CD}, {0x13430, 0x1343F},
                                  {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F}};
      const auto refused = [&](uint32_t cp) {
        for (const Span& span : REFUSED)
          if (cp >= span.first && cp <= span.last) return true;
        return false;
      };
      const auto timerWith = [](uint32_t cp) {
        char escape[32];
        if (cp < 0x10000) {
          snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned>(cp));
        } else {
          const uint32_t v = cp - 0x10000;
          snprintf(escape, sizeof(escape), "\\u%04x\\u%04x", static_cast<unsigned>(0xD800 + (v >> 10)),
                   static_cast<unsigned>(0xDC00 + (v & 0x3FF)));
        }
        return std::string("{\"page\":2,\"seconds\":5,\"label\":\"a") + escape + "b\",\"box\":[0,0,24,24]}";
      };
      for (const Span& span : REFUSED) {
        char why[64];
        for (const uint32_t cp : {span.first, span.last}) {
          snprintf(why, sizeof(why), "U+%04X is refused", static_cast<unsigned>(cp));
          bad(timerWith(cp), why);
        }
        for (const uint32_t cp : {span.first - 1, span.last + 1}) {
          if (span.first == 0 && cp == span.first - 1) continue;  // before the first code point
          if (refused(cp)) continue;
          snprintf(why, sizeof(why), "U+%04X is accepted", static_cast<unsigned>(cp));
          t = timersOf("[" + timerWith(cp) + "]");
          check(t.timers.size() == 1 && !t.timersDropped, why);
        }
      }
    }
    for (const char* printable : {"Incubate", "\\u00b5L spin", "\\u65e5\\u672c\\u8a9e", "\\ud83d\\ude00 go", "a b",
                                  "caf\\u00e9", "\\u2013 15 min"}) {
      t = timersOf("[{\"page\":2,\"seconds\":5,\"label\":\"" + std::string(printable) + "\",\"box\":[0,0,24,24]}]");
      check(t.timers.size() == 1 && !t.timersDropped, printable);
    }
    // A raw (unescaped) byte string that is not UTF-8: a stray continuation byte, an overlong form, a surrogate, a
    // cut-off sequence.
    for (const std::string& malformed :
         {std::string("a\x80z"), std::string("a\xC0\x80z"), std::string("a\xED\xA0\x80z"), std::string("a\xE2\x82"),
          std::string("a\xF5\x80\x80\x80z"), std::string("a\xFFz")}) {
      std::vector<method::BookMetadata> out;
      uint32_t newestId = 0;
      size_t skippedCount = 0;
      std::string reply =
          withTimers("[{\"page\":2,\"seconds\":5,\"label\":\"" + malformed + "\",\"box\":[0,0,24,24]}]");
      check(method::parseBooks(reply, DEVICE, out, newestId, skippedCount) && out.size() == 1 && out[0].timersDropped,
            "a label that is not well-formed UTF-8 drops the set");
    }

    // The limits themselves: 160 bytes of label (40 four-byte characters) and exactly 4096 bytes of timers are valid.
    {
      std::string emoji40;
      for (int i = 0; i < 40; ++i) emoji40 += "\\ud83d\\ude00";
      t = timersOf("[{\"page\":2,\"seconds\":5,\"label\":\"" + emoji40 + "\",\"box\":[0,0,24,24]}]");
      check(t.timers.size() == 1 && t.timers[0].label.size() == 160,
            "40 four-byte characters (160 bytes) is a valid label");
      // An array of exactly 4096 bytes when minified, found by trying the length of the last timer's label and seconds.
      std::string full;
      for (int plain = 1; plain <= 99 && full.empty(); ++plain) {
        std::string head = "[";
        for (int page = 1; page <= plain; ++page)
          head += std::string(page > 1 ? "," : "") + "{\"page\":" + std::to_string(page) +
                  ",\"seconds\":5,\"box\":[0,0,24,24]}";
        for (int labelLen = 0; labelLen <= 40 && full.empty(); ++labelLen) {
          for (int digits = 1; digits <= 6 && full.empty(); ++digits) {
            std::string last = ",{\"page\":" + std::to_string(plain + 1) + ",\"seconds\":" + std::string(1, '1') +
                               std::string(digits - 1, '0');
            if (labelLen > 0) last += ",\"label\":\"" + std::string(labelLen, 'x') + "\"";
            last += ",\"box\":[0,0,24,24]}]";
            if (head.size() + last.size() == method::MAX_TIMERS_JSON_BYTES) full = head + last;
          }
        }
      }
      check(full.size() == method::MAX_TIMERS_JSON_BYTES, "test setup: an array of exactly 4096 bytes");
      method::BookMetadata exact = timersOf(full, "200");
      check(!exact.timersDropped && !exact.timers.empty(), "exactly 4096 bytes of timers is valid");
      std::string over = full;
      over.insert(over.size() - 1, " ");  // whitespace does not count: the limit is on the minified text
      check(!timersOf(over, "200").timersDropped, "whitespace is not counted");
      std::string oneMore = full.substr(0, full.size() - 1) + ",{\"page\":151,\"seconds\":5,\"box\":[0,0,24,24]}]";
      check(oneMore.size() > method::MAX_TIMERS_JSON_BYTES && timersOf(oneMore, "200").timersDropped,
            "and one timer more is not");
    }
    // Not an array, too many, too long.
    for (const char* notArray : {"{}", "null", "5", "\"x\"", "true"}) {
      const method::BookMetadata book = timersOf(notArray);
      check(book.timers.empty() && book.timersDropped, "timers that are not an array");
    }
    // The Worker allows 100 timers and 4096 bytes: the smallest timer is about 41 bytes, so the bytes bind first.
    std::string ninety = "[", hundredOne = "[";
    for (int page = 1; page <= 101; ++page) {
      const std::string one = "{\"page\":" + std::to_string(page) + ",\"seconds\":5,\"box\":[0,0,24,24]}";
      if (page <= 90) ninety += (page > 1 ? "," : "") + one;
      hundredOne += (page > 1 ? "," : "") + one;
    }
    ninety += "]";
    hundredOne += "]";
    t = timersOf(ninety, "200");
    check(t.timers.size() == 90 && !t.timersDropped, "ninety small timers fit");
    check(timersOf(hundredOne, "200").timersDropped, "a hundred and one is too many");
    // Fewer than 100 timers but over 4096 bytes of timer data.
    std::string fat = "[";
    for (int page = 1; page <= 50; ++page)
      fat += std::string(page > 1 ? "," : "") + "{\"page\":" + std::to_string(page) + ",\"seconds\":5,\"label\":\"" +
             std::string(40, 'x') + "\",\"box\":[0,0,24,24]}";
    fat += "]";
    check(fat.size() > method::MAX_TIMERS_JSON_BYTES && timersOf(fat, "100").timersDropped,
          "more than 4096 bytes of timers");

    // The single-book reply carries timers the same way, and a book without any is as before.
    method::BookMetadata single;
    check(method::parseBook(
              reply().substr(0, reply().size() - 1) + ",\"timers\":[{\"page\":2,\"seconds\":9,\"box\":[0,0,24,24]}]}",
              DEVICE, single) &&
              single.timers.size() == 1 && single.timers[0].seconds == 9,
          "a /book reply may carry timers too");
    check(parses(reply(), single) && single.timers.empty() && !single.timersDropped, "and none is as before");
    check(parses(reply(), book) && book.timers.empty(), "metadata parsed after a book with timers starts clean");

    // ---- The list as the Worker sends it to a reader that asked for timers: the largest it can be ----
    check(std::string(method::LIST_FEATURES) == "timers", "the parser names the feature it understands");
    check(method::MAX_LIST_BYTES == 64 * 1024 && method::LIST_NESTING_LIMIT >= 6,
          "64 KB and six levels are what a reader that asks for timers must take");
    {
      // Twelve entries, each with the most timers 4096 bytes hold, every label 40 characters that all need escaping,
      // and a 120-character title of four-byte characters (480 bytes, the longest the parser keeps).
      const std::string emoji = "\xF0\x9F\x98\x80";
      std::string longTitle;
      for (int i = 0; i < 120; ++i) longTitle += emoji;
      int count = 0;
      // Labels of 40 quotes, written escaped: \" is two bytes each.
      std::string escapedLabel;
      for (int i = 0; i < 40; ++i) escapedLabel += "\\\"";
      std::string timers = "[";
      while (true) {
        const std::string one = std::string(count ? "," : "") + "{\"page\":" + std::to_string(count + 1) +
                                ",\"seconds\":359999,\"label\":\"" + escapedLabel + "\",\"box\":[456,776,24,24]}";
        if (timers.size() + one.size() + 1 > method::MAX_TIMERS_JSON_BYTES) break;
        timers += one;
        ++count;
      }
      timers += "]";
      check(timers.size() <= method::MAX_TIMERS_JSON_BYTES && timers.size() + 150 > method::MAX_TIMERS_JSON_BYTES &&
                count > 20,
            "the timers fill the Worker's 4096 bytes");
      std::string books = "{\"books\":[";
      for (int i = 0; i < 12; ++i) {
        const std::string id = "b" + std::to_string(900 - i);
        books += std::string(i ? "," : "") + "{\"book_id\":\"" + id + "\",\"kind\":\"method\",\"title\":\"" +
                 longTitle + "\",\"sha256\":\"" + HASH +
                 "\",\"byte_length\":1900000,\"page_count\":200,\"created_at\":1770000000,\"book_url\":\"/api/device/" +
                 DEVICE + "/book/" + id + "\",\"timers\":" + timers + "}";
      }
      books += "]}";
      check(books.size() > 50 * 1024 && books.size() <= method::MAX_LIST_BYTES,
            "the worst list is over 50 KB and under the 64 KB cap");
      std::vector<method::BookMetadata> out;
      uint32_t newestId = 0;
      size_t skippedCount = 0;
      check(method::parseBooks(books, DEVICE, out, newestId, skippedCount) && out.size() == 12 && skippedCount == 0 &&
                newestId == 900,
            "the worst list parses, every entry");
      bool all = true;
      for (const auto& b : out)
        all = all && b.timers.size() == static_cast<size_t>(count) && !b.timersDropped &&
              b.timers[0].label == std::string(40, '"') && std::string(b.title).size() == 480;
      check(all, "with every timer and every label of every entry, and the full titles");
      // Over the cap: refused whole, as any oversized reply.
      check(!method::parseBooks(books + std::string(method::MAX_LIST_BYTES - books.size() + 1, ' '), DEVICE, out,
                                newestId, skippedCount),
            "a list over 64 KB is refused");
    }
    // Six levels deep is the real depth of an entry with timers; one more than the limit allows is refused.
    {
      std::vector<method::BookMetadata> out;
      uint32_t newestId = 0;
      size_t skippedCount = 0;
      check(method::parseBooks(withTimers("[" + goodTimer + "]"), DEVICE, out, newestId, skippedCount) &&
                out.size() == 1 && out[0].timers.size() == 1,
            "an entry nested six deep parses");
      std::string seven = withTimers("[" + goodTimer + "]");
      seven.insert(seven.size() - 3,
                   ",\"x\":[[[[]]]]");  // inside the entry: root, books, entry and four more = seven levels
      check(method::parseBooks(seven, DEVICE, out, newestId, skippedCount) && out.size() == 1,
            "seven levels is the limit and parses");
      std::string eight = withTimers("[" + goodTimer + "]");
      eight.insert(eight.size() - 3, ",\"x\":[[[[[]]]]]");
      check(!method::parseBooks(eight, DEVICE, out, newestId, skippedCount), "eight levels is refused");
    }
    // A reply without timers parses exactly as before: no timers, nothing dropped.
    {
      std::vector<method::BookMetadata> out;
      uint32_t newestId = 0;
      size_t skippedCount = 0;
      const std::string old =
          "{\"books\":[{\"book_id\":\"b7\",\"kind\":\"table\",\"title\":\"Seeding\",\"sha256\":\"" + HASH +
          "\",\"byte_length\":2000,\"page_count\":3,\"created_at\":1770000000,\"book_url\":\"/api/device/" + DEVICE +
          "/book/b7\"}]}";
      check(method::parseBooks(old, DEVICE, out, newestId, skippedCount) && out.size() == 1 && out[0].timers.empty() &&
                !out[0].timersDropped && out[0].kind == method::Kind::Table && std::string(out[0].title) == "Seeding" &&
                newestId == 7 && skippedCount == 0,
            "a Stage A shaped reply is read as before");
    }
  }

  std::cout << checks << " Method protocol checks passed\n";
}
