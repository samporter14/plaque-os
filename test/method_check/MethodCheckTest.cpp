#include <iostream>

#include "FakePorts.h"

using fake::check;
using fake::checkEq;
using method::Phase;
using method::Result;

namespace {
const std::string DEVICE = "sams-reader";
const std::string META_ROUTE = "/api/device/" + DEVICE + "/books";
const std::string BOOK_ROUTE = "/api/device/" + DEVICE + "/book/b12";
const char* INCOMING = "/Lamina/Method/.incoming.xtc";

std::string bookPathFor(const std::string& title, const std::string& sha) {
  char out[96];
  if (!method::bookPath(title, sha, out, sizeof(out))) return "<bad>";
  return out;
}

// What the mailbox serves for one method.
struct Published {
  std::string title;
  std::string bytes;
  std::string sha;
  std::string path;
  std::string id;
  std::string kind;
  size_t pages = 8;
};

struct Rig {
  fake::FakeClock clock;
  fake::FakeWifi wifi{clock};
  fake::FakeHttp http;
  fake::FakeFiles files;
  fake::FakeHash hash;
  fake::FakeBattery battery;
  fake::FakeObserver observer;
  method::Library library;
  method::Config config;
  method::Limits limits;
  std::unique_ptr<method::Check> check;
  uint32_t startedAt = 0;

  Rig() {
    config.configured = true;
    config.https = true;
    config.deviceId = DEVICE;
    wifi.saved = {fake::FakeWifi::Network{}};
    wifi.last = 0;
  }

  std::vector<Published> mailbox;  // what GET /books lists

  static std::string route(const std::string& id) { return "/api/device/" + DEVICE + "/book/" + id; }
  static unsigned number(const std::string& id) { return static_cast<unsigned>(std::stoul(id.substr(1))); }

  // Rewrites the list reply, newest id first, as the Worker does.
  void republish() {
    std::vector<Published> sorted = mailbox;
    std::sort(sorted.begin(), sorted.end(),
              [](const Published& a, const Published& b) { return number(a.id) > number(b.id); });
    std::string body = "{\"books\":[";
    for (size_t i = 0; i < sorted.size(); ++i) {
      const Published& p = sorted[i];
      body += (i ? "," : "") + std::string("{\"book_id\":\"") + p.id + "\",\"kind\":\"" + p.kind + "\",\"title\":\"" +
              p.title + "\",\"sha256\":\"" + p.sha + "\",\"byte_length\":" + std::to_string(p.bytes.size()) +
              ",\"page_count\":" + std::to_string(p.pages) + ",\"created_at\":1790000000,\"book_url\":\"" +
              route(p.id) + "\"}";
    }
    http.routes[META_ROUTE].body = body + "]}";
  }

  // Puts one book on the mailbox (a book with the same title is replaced, as on the Worker).
  Published publish(const std::string& title, size_t size = 5000, const std::string& id = "b12", size_t pages = 8,
                    const std::string& kind = "method") {
    Published p;
    p.title = title;
    p.id = id;
    p.kind = kind;
    p.pages = pages;
    p.bytes = std::string("XTC\0", 4);
    for (size_t i = p.bytes.size(); i < size; ++i)
      p.bytes.push_back(static_cast<char>('a' + (i * 7 + number(id)) % 26));
    p.sha = fake::sha256Hex(p.bytes);
    p.path = bookPathFor(title, p.sha);
    for (size_t i = 0; i < mailbox.size(); ++i) {
      if (mailbox[i].title == title || mailbox[i].id == id) {
        http.routes.erase(route(mailbox[i].id));
        mailbox.erase(mailbox.begin() + static_cast<std::ptrdiff_t>(i));
        break;
      }
    }
    mailbox.push_back(p);
    http.routes[route(id)].body = p.bytes;
    republish();
    return p;
  }

  void make() {
    check.reset();
    method::Ports ports{clock, wifi, http, files, hash, battery, &observer};
    check = std::make_unique<method::Check>(ports, config, library, limits);
  }

  void start() {
    make();
    startedAt = clock.ms;
    check->start();
  }

  // Step like the shelf does: once per main-loop pass, 10 ms apart.
  void run(int maxSteps = 200000) {
    start();
    for (int i = 0; i < maxSteps && check->running(); ++i) {
      check->step();
      clock.ms += 10;
    }
  }

  // Step until the check is in `phase` (and has taken `extra` more steps there).
  void runUntil(Phase phase, int extra = 0) {
    start();
    for (int i = 0; i < 200000 && check->running() && check->phase() != phase; ++i) {
      check->step();
      clock.ms += 10;
    }
    for (int i = 0; i < extra && check->running(); ++i) {
      check->step();
      clock.ms += 10;
    }
  }

  uint32_t elapsed() const { return clock.ms - startedAt; }
};

// The invariants every path out of a check must keep.
void expectClean(Rig& r, const char* name) {
  const std::string n = name;
  check(!r.check->running(), (n + ": finished").c_str());
  check(r.check->phase() == Phase::Finished, (n + ": phase is Finished").c_str());
  check(!r.wifi.radioOn && !r.wifi.joined && r.wifi.connecting < 0, (n + ": Wi-Fi is off").c_str());
  check(r.wifi.offCalls >= 1, (n + ": Wi-Fi was switched off explicitly").c_str());
  check(!r.http.isOpen, (n + ": the connection is closed").c_str());
  check(!r.files.has(INCOMING), (n + ": no partial file").c_str());
  check(!r.files.sinkOpen, (n + ": no open download sink").c_str());
  check(!r.hash.active, (n + ": no open hash context").c_str());
  check(!r.clock.syncStarted, (n + ": clock sync abandoned").c_str());
  check(!r.http.resolving, (n + ": no name lookup left running").c_str());
}

void testHappyPath() {
  Rig r;
  const Published p = r.publish("Example column clean-up");
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "a new method is filed");
  expectClean(r, "happy path");
  checkEq(r.library.entries().size(), size_t{1}, "the shelf has one method");
  const method::Entry& e = r.library.entries()[0];
  checkEq(e.title, p.title, "title");
  checkEq(e.path, p.path, "path is <title> <sha8>.xtc");
  checkEq(e.sha256, p.sha, "sha256 recorded");
  checkEq(e.bookId, std::string("b12"), "book id recorded");
  checkEq(e.receivedAt, r.clock.unix, "received_at is the device clock");
  checkEq(e.pageCount, 8u, "page count from the parser");
  check(!e.opened, "a new method is new");
  checkEq(r.check->addedPath(), p.path, "the check reports what it filed");
  check(r.files.has(p.path) && r.files.files[p.path] == p.bytes, "the book is on the card, byte for byte");
  check(r.files.has(method::INDEX_PATH) && r.files.files[method::INDEX_PATH] == r.library.serialise(),
        "the index on the card is the library");
  checkEq(r.check->finishedAt(), r.clock.unix, "finished time recorded for the status line");
  checkEq(r.wifi.log, (std::vector<std::string>{"connect 0", "joined 0"}), "joined the last network");
  checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE, BOOK_ROUTE}),
          "only this device's book routes were requested");
  checkEq(r.check->percent(), 100u, "progress reached 100");
  checkEq(r.library.mark(), 12u, "the mark moved to the filed book's id");
  check(e.kind == method::Kind::Method, "kind recorded");
  for (const auto& path : r.http.opened)
    check(path.find("/book/") != std::string::npos || path == META_ROUTE, "only /books and /book/<id> are requested");
  check(std::find(r.http.opened.begin(), r.http.opened.end(), "/api/device/" + DEVICE + "/book") == r.http.opened.end(),
        "the single-book route is never requested");
  check(r.clock.beginCalls == 1, "the clock was synced once");
  check(r.elapsed() < 20000, "a healthy check is quick on a fake network");

  // Phases, in order, each with its duration.
  const auto& ev = r.observer.events;
  const auto at = [&](const std::string& prefix) {
    for (size_t i = 0; i < ev.size(); ++i)
      if (ev[i].compare(0, prefix.size(), prefix) == 0) return static_cast<int>(i);
    return -1;
  };
  check(at("enter wifi") < at("enter clock") && at("enter clock") < at("enter mailbox") &&
            at("enter mailbox") < at("enter download") && at("enter download") < at("enter verify"),
        "phases run in the specified order");
  check(at("leave download") >= 0 && ev.back().find("new method filed") != std::string::npos,
        "the last phase logs its outcome");
}

void testProgressIsReportedInTens() {
  Rig r;
  r.publish("Long", 30000);
  r.http.routes[BOOK_ROUTE].slice = 400;
  r.make();
  r.check->start();
  std::vector<uint32_t> seen;
  uint32_t lastRevision = r.check->revision();
  for (int i = 0; i < 100000 && r.check->running(); ++i) {
    r.check->step();
    r.clock.ms += 10;
    if (r.check->phase() == Phase::Download && r.check->revision() != lastRevision) seen.push_back(r.check->percent());
    lastRevision = r.check->revision();
  }
  check(r.check->result() == Result::NewMethod, "long book downloads");
  check(!seen.empty(), "progress changes are published");
  bool tens = true, increasing = true;
  for (size_t i = 0; i < seen.size(); ++i) {
    tens = tens && seen[i] % 10 == 0;
    increasing = increasing && (i == 0 || seen[i] > seen[i - 1]);
  }
  check(tens && increasing, "progress moves in 10 % steps and never repeats");
  checkEq(seen.back(), 100u, "and ends at 100");
  check(seen.size() <= 12, "at most about one repaint per step, not per chunk");
}

void testNothingNew() {
  {
    Rig r;
    const Published p = r.publish("Held");
    method::Entry e;
    e.title = p.title;
    e.path = p.path;
    e.sha256 = p.sha;
    e.pageCount = 8;
    r.library.add(e);
    r.files.put(p.path, p.bytes);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate),
            "up to date when the version is held");
    expectClean(r, "held");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "no download");
    check(r.library.entries().size() == 1, "library unchanged");
  }
  {
    Rig r;  // the mailbox holds nothing
    r.http.routes[META_ROUTE].body = "{\"books\":[]}";
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate), "an empty mailbox is up to date");
    expectClean(r, "empty mailbox");
  }
  {
    Rig r;  // the user removed this book: its id is at or below the mark, so it stays removed
    const Published p = r.publish("Removed on purpose");
    r.library.advanceMark(12);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate),
            "a removed book is not fetched again");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "and not downloaded");
    check(r.library.entries().empty() && !r.files.has(p.path), "and stays off the shelf");
    checkEq(r.library.mark(), 12u, "the mark is unchanged");
  }
  {
    Rig r;  // the file name matches (a book adopted from the card)
    const Published p = r.publish("Adopted");
    method::Entry e;
    e.title = p.title;
    e.path = p.path;
    e.pageCount = 8;
    r.library.add(e);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate),
            "an adopted copy of this version counts");
  }
}

void testNotConfiguredAndBattery() {
  {
    Rig r;
    r.config.configured = false;
    r.publish("X");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NotConfigured), "not configured");
    check(r.wifi.log.empty() && r.http.opened.empty(), "nothing touched the network");
    check(!r.wifi.radioOn, "radio stays off");
  }
  {
    Rig r;
    r.battery.level = 14;
    r.publish("X");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::LowBattery), "14 % is too low");
    check(r.wifi.log.empty() && r.http.opened.empty() && r.clock.beginCalls == 0, "no radio, no network");
    check(!r.wifi.radioOn, "radio stays off");
  }
  {
    Rig r;
    r.battery.level = 15;
    r.publish("X");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "15 % is enough");
  }
}

void testWifiFallbacks() {
  using Network = fake::FakeWifi::Network;
  {
    // The last network is out of range: 4 s, then a scan, then the other saved network.
    Rig r;
    r.publish("Lab");
    r.wifi.saved = {Network{60000, false, false, -50}, Network{1000, false, true, -70}};
    r.wifi.last = 0;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "joined the lab network");
    checkEq(r.wifi.log, (std::vector<std::string>{"connect 0", "abort 0", "scan", "connect 1", "joined 1"}),
            "last SSID, scan, then the visible saved network");
    checkEq(r.wifi.last, 1, "the lab network is remembered as the one to try first");
    expectClean(r, "wifi fallback");
  }
  {
    // The last SSID is given 4 s, not longer.
    Rig r;
    r.publish("Lab");
    r.wifi.saved = {Network{3900, false, true, -50}};
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "joins just inside 4 s");
    check(r.wifi.log.size() == 2, "no scan needed");
  }
  {
    Rig r;
    r.publish("Lab");
    r.wifi.saved = {Network{4500, false, true, -50}};  // too slow for the first try, fine for the second
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a slow last network joins on the second try");
    checkEq(r.wifi.log, (std::vector<std::string>{"connect 0", "abort 0", "scan", "connect 0", "joined 0"}),
            "the scan pass gives it the full 7 s");
  }
  {
    // A radio failure ends the attempt at once instead of waiting out the 4 s.
    Rig r;
    r.publish("Lab");
    r.wifi.saved = {Network{800, true, true, -50}, Network{800, false, true, -80}};
    r.runUntil(Phase::Clock);
    check(r.elapsed() < 4000, "a failed join does not wait for the timeout");
    checkEq(r.wifi.joinedIndex, 1, "went on to the second network");
  }
  {
    // Strongest saved network first.
    Rig r;
    r.publish("Lab");
    r.wifi.saved = {Network{60000, false, false, -50}, Network{800, false, true, -80}, Network{800, false, true, -40}};
    r.run();
    checkEq(r.wifi.joinedIndex, 2, "the strongest visible saved network is tried first");
  }
  {
    // No network ever joins: the phase ends within its hard limit, radio off.
    Rig r;
    r.publish("Lab");
    r.wifi.saved = {Network{600000, false, true, -50}, Network{600000, false, true, -60},
                    Network{600000, false, true, -70}, Network{600000, false, true, -80},
                    Network{600000, false, true, -90}};
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::WifiFailed), "Wi-Fi failed");
    check(r.elapsed() <= r.limits.wifiTotalMs + 100, "within the 30 s limit");
    check(r.http.opened.empty(), "no request without Wi-Fi");
    expectClean(r, "wifi total timeout");
  }
  {
    Rig r;
    r.wifi.saved.clear();
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::WifiFailed), "no saved networks");
    check(r.elapsed() < 100, "immediately");
    expectClean(r, "no saved networks");
  }
  {
    Rig r;
    r.wifi.scanFails = true;
    r.wifi.saved = {Network{600000, false, true, -50}};
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::WifiFailed), "a failed scan fails the phase");
    expectClean(r, "scan failed");
  }
  {
    Rig r;
    r.wifi.scanTakesMs = 600000;
    r.wifi.saved = {Network{600000, false, true, -50}};
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::WifiFailed),
            "a scan that never ends fails the phase");
    check(r.elapsed() <= r.limits.wifiLastMs + r.limits.wifiScanMs + 100, "within the scan limit");
    expectClean(r, "scan timeout");
  }
  {
    Rig r;
    r.wifi.saved = {Network{600000, false, false, -50}, Network{600000, false, false, -60}};
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::WifiFailed),
            "saved networks all out of range");
    expectClean(r, "none visible");
  }
  {
    // Already connected (something else left Wi-Fi up): no join, still switched off at the end.
    Rig r;
    r.publish("Lab");
    r.wifi.joined = true;
    r.wifi.radioOn = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "works on an existing connection");
    check(r.wifi.log.empty(), "no join was attempted");
    expectClean(r, "already connected");
  }
}

void testClockPhase() {
  {
    Rig r;
    r.publish("X");
    r.clock.ready = true;  // synced within the last 90 s
    r.clock.syncedAt = r.clock.ms;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "recent sync is reused");
    checkEq(r.clock.beginCalls, 0, "no second sync");
  }
  {
    Rig r;
    r.publish("X");
    r.clock.syncNever = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "no clock, no TLS");
    check(r.elapsed() <= r.limits.clockMs + 4000 + 100, "within the clock limit");
    check(r.http.opened.empty(), "no request without a clock");
    checkEq(r.clock.abortCalls, 1, "the sync was abandoned");
    expectClean(r, "clock timeout");
  }
  {
    Rig r;
    r.publish("X");
    r.clock.syncFails = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "a failed sync fails the phase");
    expectClean(r, "clock failed");
  }
  {
    // A sync that succeeds never asks the retained clock.
    Rig r;
    r.publish("X");
    r.clock.retainedTrusted = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "a sync that works is used");
    checkEq(r.clock.retainedCalls, 0, "and the retained clock is never asked");
  }
  {
    // S1: a network that blocks NTP. SNTP fails; the retained clock, trusted, stands in and the shelf carries on.
    Rig r;
    r.publish("X");
    r.clock.syncFails = true;
    r.clock.retainedTrusted = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a failed sync falls back on a trusted retained clock");
    checkEq(r.clock.retainedCalls, 1, "asked once");
    check(!r.http.opened.empty(), "and the request was made");
    expectClean(r, "retained clock after a failed sync");
  }
  {
    // The sync never answers at all: after the 20 s limit the retained clock is asked, not before.
    Rig r;
    r.publish("X");
    r.clock.syncNever = true;
    r.clock.retainedTrusted = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a sync that never answers falls back on a trusted retained clock");
    checkEq(r.clock.retainedCalls, 1, "asked once, when the limit passed");
    check(r.elapsed() >= r.limits.clockMs, "not before the clock limit");
    expectClean(r, "retained clock after a silent sync");
  }
  {
    // A retained clock that is not trusted (a reset, a sync too old) changes nothing: no clock, no request.
    Rig r;
    r.publish("X");
    r.clock.syncFails = true;
    r.clock.retainedTrusted = false;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "no trusted clock, no TLS");
    checkEq(r.clock.retainedCalls, 1, "the retained clock was asked");
    check(r.http.opened.empty(), "and no request was made");
    expectClean(r, "retained clock not trusted");
  }
  {
    // A local-development http origin needs no clock.
    Rig r;
    r.config.https = false;
    r.clock.syncNever = true;
    r.http.heapReady = false;  // and no TLS heap either
    r.publish("X");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "plain http skips the clock");
    checkEq(r.clock.beginCalls, 0, "and does not sync");
  }
}

// The clock phase is a poll loop. The shelf calls step() once per main-loop pass and reads its input between
// calls, so a sync that takes seconds must not hold any single step: every step asks syncStatus() once and returns.
// Cancel and the 20 s limit then work at any instant of the sync, and an abort after a finished sync is harmless.
void testClockSyncNeverHoldsTheLoop() {
  // How long the longest single step() took on the fake clock, over the whole Clock phase of one run.
  struct Run {
    uint32_t longestStepMs = 0;
    bool oneRead = true;  // each step in the phase called syncStatus() exactly once
    int clockSteps = 0;
  };
  const auto runThroughClock = [](Rig& r) {
    Run run;
    r.make();
    r.startedAt = r.clock.ms;
    r.check->start();
    for (int i = 0; i < 200000 && r.check->running(); ++i) {
      const bool inClock = r.check->phase() == Phase::Clock;
      const uint32_t before = r.clock.ms;
      const int polls = r.clock.statusPolls;
      r.check->step();
      if (inClock) {
        ++run.clockSteps;
        run.longestStepMs = std::max(run.longestStepMs, static_cast<uint32_t>(r.clock.ms - before));
        run.oneRead = run.oneRead && r.clock.statusPolls == polls + 1;
      }
      r.clock.ms += 10;
    }
    return run;
  };

  {
    // A 12 s sync, far past the 100 ms bound on how long input may go unread, still ends well.
    Rig r;
    r.publish("X");
    r.clock.syncAfterMs = 12000;
    const Run run = runThroughClock(r);
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a slow sync inside the limit succeeds");
    check(run.clockSteps >= 1190, "the check kept stepping through the whole sync");
    check(run.oneRead, "each step asked syncStatus() once");
    check(run.longestStepMs <= 100, "no step waited for the sync");
    checkEq(r.clock.beginCalls, 1, "one sync");
    check(r.clock.clockReady(), "finishing the check (abortSync after the sync ended) keeps the clock usable");
    expectClean(r, "slow sync");
  }
  {
    // Control: an adapter that waits inside syncStatus() is what this measurement exists to catch.
    Rig r;
    r.publish("X");
    r.clock.syncAfterMs = 12000;
    r.clock.blockingSync = true;
    const Run run = runThroughClock(r);
    check(run.longestStepMs >= 12000 - 10, "a blocking adapter would show up as a 12 s step");
  }
  {
    // The 20 s limit holds to the step: a sync ready just inside it is used, one just outside it is not.
    Rig inside;
    inside.publish("X");
    inside.clock.syncAfterMs = 19900;
    runThroughClock(inside);
    checkEq(static_cast<int>(inside.check->result()), static_cast<int>(Result::NewMethod), "a sync at 19.9 s is used");

    Rig outside;
    outside.publish("X");
    outside.clock.syncAfterMs = 20100;
    const Run run = runThroughClock(outside);
    checkEq(static_cast<int>(outside.check->result()), static_cast<int>(Result::ServerError),
            "a sync at 20.1 s is too late");
    check(run.clockSteps <= static_cast<int>(outside.limits.clockMs / 10) + 2, "and the phase gave up at the limit");
    check(outside.http.opened.empty(), "no request without a clock");
    checkEq(outside.clock.abortCalls, 1, "the late sync was abandoned once");
    expectClean(outside, "late sync");
  }

  // Cancel at any instant of a sync that never finishes: back out within that step, sync abandoned once, Wi-Fi off.
  for (const uint32_t after : {0u, 10u, 90u, 100u, 110u, 1000u, 5000u, 11990u, 19990u}) {
    Rig r;
    r.publish("X");
    r.clock.syncNever = true;
    r.runUntil(Phase::Clock, static_cast<int>(after / 10));
    const std::string n = "cancel " + std::to_string(after) + " ms into the clock sync";
    check(r.check->running() && r.check->phase() == Phase::Clock, (n + ": still syncing").c_str());
    const uint32_t before = r.clock.ms;
    r.check->cancel();
    checkEq(r.clock.ms, before, (n + ": the cancel took no time").c_str());
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Cancelled), (n + ": cancelled").c_str());
    checkEq(r.clock.abortCalls, 1, (n + ": the sync was abandoned once").c_str());
    check(r.http.opened.empty(), (n + ": no request was made").c_str());
    expectClean(r, n.c_str());
  }
}

// Releasing the font caches before a request needs the render lock, which the render task holds while it draws.
// The loop only tries: the port answers Wait and the check asks again on its next step, so input keeps being read
// and the phase limit bounds the wait.
void testWaitingForTheRenderTask() {
  {
    Rig r;
    r.publish("X");
    r.http.readyWaits = 40;  // a render in progress for about 400 ms
    const uint32_t longest = [&] {
      uint32_t worst = 0;
      r.make();
      r.startedAt = r.clock.ms;
      r.check->start();
      for (int i = 0; i < 200000 && r.check->running(); ++i) {
        const uint32_t before = r.clock.ms;
        r.check->step();
        worst = std::max(worst, static_cast<uint32_t>(r.clock.ms - before));
        r.clock.ms += 10;
      }
      return worst;
    }();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a short wait does not fail the check");
    check(r.http.readyCalls >= 42,
          "it asked again on every step until the render task was done, then once per request");
    check(longest <= 100, "waiting never held a step");
    expectClean(r, "short wait");
  }
  {
    // Never free (the render task never lets go): the mailbox phase limit ends it, as it ends every other wait.
    Rig r;
    r.publish("X");
    r.http.readyWaits = -1;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "the mailbox limit ends a wait for ever");
    check(r.http.opened.empty(), "no request was made");
    check(r.elapsed() <= r.limits.mailboxMs + 3000, "within the mailbox limit (after joining and syncing)");
    expectClean(r, "endless wait");
  }
  {
    // The same in the download phase: the list was read, then the render task never lets go.
    Rig r;
    r.publish("X");
    r.limits.downloadMs = 5000;
    r.http.onOpen = [&r](const std::string& path) {
      if (path == META_ROUTE) r.http.readyWaits = -1;
    };
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "the download limit ends a wait for ever");
    checkEq(r.http.opened.size(), size_t{1}, "only the list was requested");
    expectClean(r, "endless wait in the download phase");
  }
  {
    // Not enough heap is still a Memory result, and not a wait.
    Rig r;
    r.publish("X");
    r.http.heapReady = false;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Memory), "no heap is Memory");
    expectClean(r, "no heap");
  }
}

// Each phase reports the longest gap between two steps: how long the main loop went without reading input. A
// download made of 10 ms passes says 10; one slow pass inside it (a read that waited, a refresh) shows.
void testSlowestPassIsReported() {
  {
    Rig r;
    r.publish("X", 40000);
    r.http.routes[BOOK_ROUTE].slice = 500;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "filed");
    for (const char* phase : {"wifi", "clock", "mailbox", "download", "verify"}) {
      check(r.observer.slowestPass.count(phase) != 0, (std::string(phase) + ": reported").c_str());
      checkEq(r.observer.slowestPass[phase], 10u, (std::string(phase) + ": ten-millisecond passes").c_str());
    }
  }
  {
    // One pass in the middle of the download takes 160 ms longer than the rest.
    Rig r;
    r.publish("X", 40000);
    r.http.routes[BOOK_ROUTE].slice = 500;
    r.make();
    r.startedAt = r.clock.ms;
    r.check->start();
    int downloadSteps = 0;
    for (int i = 0; i < 200000 && r.check->running(); ++i) {
      r.check->step();
      r.clock.ms += 10;
      if (r.check->phase() == Phase::Download && ++downloadSteps == 30) r.clock.ms += 160;
    }
    checkEq(r.observer.slowestPass["download"], 170u, "the slow pass is the one reported");
    checkEq(r.observer.slowestPass["wifi"], 10u, "and only for its own phase");
    checkEq(r.observer.slowestPass["verify"], 10u, "later phases start again");
  }
  {
    // A check that is started again does not carry the old gap into its first step.
    Rig r;
    r.publish("X");
    r.run();
    r.clock.ms += 600000;  // ten minutes later
    r.check->start();
    for (int i = 0; i < 2000 && r.check->running(); ++i) {
      r.check->step();
      r.clock.ms += 10;
    }
    checkEq(r.observer.slowestPass["wifi"], 10u, "the pause between two checks is not a slow pass");
  }
}

// The download limit is the request's own allowance plus the time the book needs at the slowest acceptable speed,
// so a near-maximum book on a slow link is not failed at 99 %. The stall limit is what ends a dead connection.
void testDownloadLimitScalesWithTheBook() {
  {
    // A full-size book (1.9 MB) at 10 KB/s takes 190 s. The old fixed limit of 180 s failed it every time.
    Rig r;
    r.publish("Big", method::MAX_BOOK_BYTES);
    r.http.routes[BOOK_ROUTE].slice = 100;  // 100 bytes per 10 ms step
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a full-size book at 10 KB/s is filed");
    check(r.elapsed() > 180000, "and it took longer than the old limit");
    expectClean(r, "full-size book");
  }
  {
    // Slower than the slowest acceptable speed (5 KB/s against 6 KB/s): the limit still ends it, near the end.
    Rig r;
    r.publish("Big", method::MAX_BOOK_BYTES);
    r.http.routes[BOOK_ROUTE].slice = 50;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed), "too slow still fails");
    const uint32_t allowed = r.limits.downloadMs + method::MAX_BOOK_BYTES * 1000ull / r.limits.downloadBytesPerSec;
    check(r.elapsed() >= allowed - 2000 && r.elapsed() <= allowed + 8000, "at the size-scaled limit");
    expectClean(r, "too slow");
  }
  {
    // A dead connection in the middle of a full-size book is ended by the 5 s stall limit, not by the long total.
    Rig r;
    r.publish("Big", method::MAX_BOOK_BYTES);
    r.http.routes[BOOK_ROUTE].slice = 2048;
    r.http.routes[BOOK_ROUTE].stallAt = 1000000;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed), "a stall fails");
    check(r.elapsed() < 30000, "within seconds of the stall");
    expectClean(r, "stall in a big book");
  }
  {
    // A small book keeps the request's own allowance: nothing arriving at all ends at downloadMs plus its few seconds.
    Rig r;
    r.publish("X", 5000);
    r.limits.stallMs = 10000000;  // so only the total limit can end it
    r.http.routes[BOOK_ROUTE].idleBefore = 100000;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "a silent server ends at the total limit");
    check(r.elapsed() >= 60000 && r.elapsed() <= 60000 + 4000 + 2000, "which for a small book is about a minute");
    expectClean(r, "silent server");
  }
}

// A chunked reply has no Content-Length (esp_http_client reads that as 0), and a proxy can change one. Neither says
// anything about the book, so the mark must not advance past it: the next check asks again, and it succeeds.
void testNoContentLengthDoesNotConsumeTheBook() {
  Rig r;
  r.publish("X", 5000, "b12");
  r.http.routes[BOOK_ROUTE].length = 0;
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed), "length 0 fails the download");
  checkEq(r.library.mark(), 0u, "the mark did not move");
  check(r.library.entries().empty() && !r.files.has(INCOMING), "nothing filed, no partial file");
  expectClean(r, "length 0");

  r.http.routes[BOOK_ROUTE].length = -2;  // the proxy behaves again
  r.check->start();
  for (int i = 0; i < 20000 && r.check->running(); ++i) {
    r.check->step();
    r.clock.ms += 10;
  }
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the next check files it");
  checkEq(r.library.mark(), 12u, "and only then does the mark move");

  // A missing length (negative) was already a failed download.
  Rig missing;
  missing.publish("X", 5000, "b12");
  missing.http.routes[BOOK_ROUTE].length = -1;
  missing.run();
  checkEq(static_cast<int>(missing.check->result()), static_cast<int>(Result::DownloadFailed),
          "no length fails the download");
  checkEq(missing.library.mark(), 0u, "and does not move the mark");
}

// Rows of the list this reader cannot read (damaged, or from a newer Worker) are counted. With nothing filed they
// are not "Up to date": the status says so. A good new book beside them is still filed.
void testUnreadableRowsAreReported() {
  const auto skippedRow = [](Rig& r) {
    // A row with an unknown kind, from a Worker newer than this reader.
    std::string body = r.http.routes[META_ROUTE].body;
    const std::string row =
        "{\"book_id\":\"b99\",\"kind\":\"hologram\",\"title\":\"New\",\"sha256\":\"" + std::string(64, 'a') +
        "\",\"byte_length\":10,\"page_count\":2,\"created_at\":1790000000,\"book_url\":\"" + Rig::route("b99") + "\"}";
    body.insert(body.find('[') + 1, row + (body.find('[') + 1 == body.rfind(']') ? "" : ","));
    r.http.routes[META_ROUTE].body = body;
  };
  {
    Rig r;  // only an unreadable row
    r.publish("X");
    r.mailbox.clear();
    r.http.routes[META_ROUTE].body = "{\"books\":[]}";
    skippedRow(r);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UnreadableList),
            "only unreadable rows: not up to date");
    checkEq(r.check->skippedRows(), size_t{1}, "one row skipped");
    check(!method::reachedMailbox(Result::UnreadableList), "it is not a check that excuses the next visit");
    expectClean(r, "only unreadable rows");
  }
  {
    Rig r;  // a good book the card already holds, and an unreadable row: nothing new, but not up to date either
    r.publish("Have", 5000, "b12");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "first check files it");
    skippedRow(r);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UnreadableList),
            "nothing new and a row it cannot read");
    check(r.library.entries().size() == 1, "the book on the card is untouched");
  }
  {
    Rig r;  // a new good book beside an unreadable row: the book is filed, and that is the result
    r.publish("Good", 5000, "b12");
    skippedRow(r);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "a filed book wins");
    checkEq(r.check->skippedRows(), size_t{1}, "the unreadable row is still counted");
  }
  {
    Rig r;  // nothing unreadable: unchanged
    r.publish("Good", 5000, "b12");
    r.run();
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate), "a clean list is up to date");
    checkEq(r.check->skippedRows(), size_t{0}, "nothing skipped");
  }
}

// Every request starts with a name lookup that the check polls and can give up on: esp_http_client's own getaddrinfo
// has no timeout and no way out, so a network with no working DNS would hold Back, Home and sleep.
void testNameLookupBeforeEveryRequest() {
  {
    Rig r;
    r.publish("One", 5000, "b10");
    r.publish("Two", 5000, "b11");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "filed with lookups in between");
    checkEq(r.http.sequence,
            (std::vector<std::string>{"resolve", "open " + META_ROUTE, "resolve", "open " + Rig::route("b10"),
                                      "resolve", "open " + Rig::route("b11")}),
            "a lookup, then the request, for the list and for each book");
    checkEq(r.http.resolveBegins, 3, "three lookups");
    expectClean(r, "lookups");
  }
  {
    // A slow lookup (2.5 s) is polled step by step: no step waits, and the request follows.
    Rig r;
    r.publish("X");
    r.http.resolvePolls = 250;
    uint32_t worst = 0;
    r.make();
    r.startedAt = r.clock.ms;
    r.check->start();
    for (int i = 0; i < 200000 && r.check->running(); ++i) {
      const uint32_t before = r.clock.ms;
      r.check->step();
      worst = std::max(worst, static_cast<uint32_t>(r.clock.ms - before));
      r.clock.ms += 10;
    }
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "a slow lookup is waited for");
    check(worst <= 100, "without holding a step");
    expectClean(r, "slow lookup");
  }
  {
    // A lookup that never answers ends at its own limit, with nothing requested.
    Rig r;
    r.publish("X");
    r.http.resolveNever = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "no answer: the mailbox cannot be reached");
    check(r.http.opened.empty(), "no request was made");
    check(r.elapsed() <= 4000 + r.limits.resolveMs + 600, "within the lookup's limit (after joining and syncing)");
    check(r.elapsed() >= r.limits.resolveMs, "and not before it");
    checkEq(r.http.resolveAborts, 1, "the lookup was abandoned once");
    expectClean(r, "lookup never answers");
  }
  {
    // A lookup that fails (no such host, no DNS server) ends at once.
    Rig r;
    r.publish("X");
    r.http.resolveFails = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "a failed lookup");
    check(r.http.opened.empty(), "no request was made");
    check(r.elapsed() < 4000, "and it ended at once");
    expectClean(r, "lookup fails");
  }
  {
    // The same in the download phase: the list was read, then the name stops resolving.
    Rig r;
    r.publish("X");
    r.http.onOpen = [&r](const std::string& path) {
      if (path == META_ROUTE) r.http.resolveFails = true;
    };
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "a failed lookup before a download");
    checkEq(r.http.opened.size(), size_t{1}, "only the list was requested");
    expectClean(r, "lookup fails before a download");
  }
  {
    // Cancel while the lookup is pending, in either phase: abandoned, nothing left running.
    Rig r;
    r.publish("X");
    r.http.resolveNever = true;
    r.runUntil(Phase::Mailbox, 30);
    check(r.http.resolving, "a lookup is pending");
    r.check->cancel();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Cancelled), "cancelled");
    checkEq(r.http.resolveAborts, 1, "the lookup was abandoned once");
    expectClean(r, "cancel during the lookup");
  }
  {
    Rig r;  // plain http (a local development origin) is looked up the same way
    r.config.https = false;
    r.publish("X");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "plain http works");
    checkEq(r.http.resolveBegins, 2, "with a lookup per request");
  }
}

// An index that could not be read leaves the library read-only: nothing could be filed, so the check must not start
// the radio, ask the mailbox or download a book it cannot keep.
void testReadOnlyLibraryDoesNotCheck() {
  Rig r;
  r.publish("X");
  r.files.put(method::INDEX_PATH, "{\"version\":1,\"mark\":5,\"methods\":[]}\n");
  r.files.failRead.insert(method::INDEX_PATH);
  const auto report = r.library.load(r.files);
  check(report.indexUnreadable && r.library.readOnly(), "precondition: the index cannot be read");
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::StorageFailed),
          "the check refuses with the storage result");
  check(r.wifi.log.empty() && r.http.opened.empty() && r.clock.beginCalls == 0, "no radio, no mailbox, no clock");
  check(r.http.resolveBegins == 0 && !r.files.has(INCOMING), "no lookup and no partial file");
  check(!method::reachedMailbox(r.check->result()), "and it does not count as a check");
  checkEq(r.library.mark(), 0u, "nothing was filed and the mark did not move");
  check(r.files.files[method::INDEX_PATH].find("\"mark\":5") != std::string::npos,
        "the index on the card is untouched");
  expectClean(r, "read-only library");

  // The next visit with a readable index checks as usual.
  r.files.failRead.clear();
  r.library.load(r.files);
  check(!r.library.readOnly(), "readable again");
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "and the check runs");
}

void testMailboxFailures() {
  {
    Rig r;
    r.http.routes[META_ROUTE].open = method::HttpOpen::Unauthorized;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::AuthRefused),
            "401 or 403 is a refused token");
    check(r.http.opened.size() == 1, "one request");
    expectClean(r, "token refused");
  }
  {
    Rig r;
    r.http.routes[META_ROUTE].open = method::HttpOpen::Failed;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "a failed request is a server error");
    expectClean(r, "request failed");
  }
  {
    Rig r;  // no route at all
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "unreachable");
    expectClean(r, "unreachable");
  }
  {
    Rig r;
    r.http.routes[META_ROUTE].open = method::HttpOpen::NoMemory;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Memory), "no memory for the session");
    expectClean(r, "no memory");
  }
  {
    Rig r;
    r.publish("X");
    r.http.heapReady = false;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Memory), "not enough heap to start TLS");
    check(r.http.opened.empty(), "the session was never started");
    expectClean(r, "heap guard");
  }
  {
    Rig r;
    r.http.routes[META_ROUTE].body = "this is not json";
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "an unreadable reply");
    expectClean(r, "bad reply");
  }
  {
    Rig r;  // an entry that points the book route at another device is skipped, never requested
    r.publish("X");
    std::string body = r.http.routes[META_ROUTE].body;
    const size_t at = body.find("sams-reader");
    body.replace(at, 11, "other-device");
    r.http.routes[META_ROUTE].body = body;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UnreadableList),
            "a book_url for another device is skipped, and said so");
    check(r.http.opened.size() == 1, "and never requested");
    check(r.library.entries().empty() && r.library.mark() == 0, "nothing filed, mark untouched");
    expectClean(r, "foreign book_url");
  }
  {
    Rig r;  // one damaged entry does not hide the good one beside it
    r.publish("Good", 5000, "b12");
    r.publish("Bad", 5000, "b13", 8, "sheet");  // an unknown kind
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the good entry is still filed");
    check(r.library.find(bookPathFor("Good", r.mailbox[0].sha)) != nullptr && r.library.entries().size() == 1,
          "only the good entry");
    checkEq(r.library.mark(), 12u, "the mark does not jump past the damaged entry's id");
    expectClean(r, "damaged entry");
  }
  {
    Rig r;  // a reply that is valid JSON but not a list
    r.http.routes[META_ROUTE].body = "{\"book_id\":null}";
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "the old single-book reply is not a list");
    expectClean(r, "not a list");
  }
  {
    Rig r;  // the reply is bigger than any list can be
    r.http.routes[META_ROUTE].body = std::string(13 * 1024, ' ');
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "an oversized reply is refused");
    expectClean(r, "oversized reply");
  }
  {
    Rig r;  // a server that goes quiet
    r.publish("X");
    r.http.routes[META_ROUTE].stallAt = 10;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "a stalled reply fails");
    check(r.elapsed() <= r.limits.clockMs + 4000 + r.limits.stallMs + 600, "after the 5 s stall limit");
    expectClean(r, "mailbox stall");
  }
  {
    Rig r;  // a server that dribbles one byte per read: the 20 s total limit applies
    r.publish("X");
    r.http.routes[META_ROUTE].body +=
        std::string(2040 - r.http.routes[META_ROUTE].body.size(), ' ');  // valid JSON, 2040 bytes
    r.http.routes[META_ROUTE].slice = 1;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "a dribbling reply hits the total limit");
    check(r.elapsed() <= 4000 + 1000 + r.limits.mailboxMs + 1000, "within the mailbox limit");
    expectClean(r, "mailbox total timeout");
  }
  {
    Rig r;  // a device id that cannot be put in a URL
    r.config.deviceId = "bad id";
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "an unusable device id");
    check(r.http.opened.empty(), "no request is made");
    expectClean(r, "bad device id");
  }
}

void testSpace() {
  const size_t spare = 256 * 1024;
  {
    Rig r;
    const Published p = r.publish("Big", 5000);
    r.files.free = p.bytes.size() + spare - 1;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::LowSpace), "one byte short");
    checkEq(r.check->neededBytes(), static_cast<uint64_t>(p.bytes.size() + spare), "reports what is needed");
    check(!r.files.has(INCOMING) && r.files.count(std::string("open ") + INCOMING) == 0, "no file was started");
    check(r.http.opened.size() == 1, "no download was requested");
    expectClean(r, "low space");
  }
  {
    Rig r;
    const Published p = r.publish("Big", 5000);
    r.files.free = p.bytes.size() + spare;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "exactly enough");
  }
  {
    Rig r;
    r.publish("Big", 5000);
    r.files.free = 0;  // unknown
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "unknown free space goes ahead");
  }
}

void testDownloadFailures() {
  {
    Rig r;
    r.publish("X");
    r.http.routes[BOOK_ROUTE].open = method::HttpOpen::Failed;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "book request failed (410 or similar)");
    expectClean(r, "book open failed");
  }
  {
    Rig r;
    r.publish("X");
    r.http.routes[BOOK_ROUTE].open = method::HttpOpen::Unauthorized;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::AuthRefused),
            "token refused on the book route");
    expectClean(r, "book token");
  }
  {
    Rig r;  // the server stops sending halfway
    r.publish("X", 6000);
    r.http.routes[BOOK_ROUTE].stallAt = 2800;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed), "a stalled download fails");
    expectClean(r, "download stall");
    check(r.files.count(std::string("remove ") + INCOMING) >= 1, "the partial file was removed");
    checkEq(r.hash.abandonCalls, 1, "the hash was released");
    check(r.library.entries().empty(), "nothing was filed");
  }
  {
    Rig r;  // the connection drops
    r.publish("X");
    r.http.routes[BOOK_ROUTE].failAtEnd = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "a dropped connection fails");
    expectClean(r, "download dropped");
  }
  {
    Rig r;  // slower than the total limit: 3 KB/s, where the limit allows 3 s plus the book at 6 KB/s (9.7 s)
    r.publish("X", 40000);
    r.http.routes[BOOK_ROUTE].slice = 30;
    r.limits.downloadMs = 3000;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "the total download limit applies");
    check(r.elapsed() <= 4000 + 9667 + 500, "and ends on time");
    expectClean(r, "download timeout");
  }
  {
    Rig r;  // idle before the first byte, but not too long
    r.publish("X");
    r.http.routes[BOOK_ROUTE].idleBefore = 100;  // 1 s of nothing
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "a short pause is not a stall");
  }
  {
    Rig r;  // Content-Length disagrees with the metadata
    r.publish("X");
    r.http.routes[BOOK_ROUTE].length = static_cast<int64_t>(r.http.routes[BOOK_ROUTE].body.size()) - 1;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "a length that contradicts the metadata fails the download");
    checkEq(r.library.mark(), 0u, "and the mark does not move past a book nothing was proven about");
    check(r.library.entries().empty(), "nothing filed");
    expectClean(r, "length mismatch");
  }
  {
    Rig r;  // the server sends more than it announced
    r.publish("X");
    r.http.routes[BOOK_ROUTE].length = static_cast<int64_t>(r.http.routes[BOOK_ROUTE].body.size());
    r.http.routes[BOOK_ROUTE].body += "extra";
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::VerifyFailed), "extra bytes are refused");
    expectClean(r, "too many bytes");
  }
  {
    Rig r;
    r.publish("X");
    r.files.failSinkWrite = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::StorageFailed), "a write error");
    expectClean(r, "write error");
  }
  {
    Rig r;
    r.publish("X");
    r.files.failOpenSink = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::StorageFailed),
            "cannot create the download file");
    check(r.http.opened.size() == 1, "and no download was requested");
    expectClean(r, "open error");
  }
  {
    Rig r;
    r.publish("X");
    r.files.failEnsure = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::StorageFailed), "cannot create the folder");
    expectClean(r, "mkdir error");
  }
  {
    Rig r;
    r.publish("X");
    r.hash.failBegin = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Memory), "no hash context");
    expectClean(r, "hash error");
  }
}

void testVerifyFailures() {
  {
    Rig r;  // same length, different bytes: the checksum catches it
    const Published p = r.publish("X");
    std::string tampered = p.bytes;
    tampered[100] = tampered[100] == 'z' ? 'y' : 'z';
    r.http.routes[BOOK_ROUTE].body = tampered;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::VerifyFailed), "a wrong checksum is refused");
    expectClean(r, "checksum");
    check(!r.files.has(p.path) && r.library.entries().empty(), "nothing was installed");
  }
  {
    Rig r;  // right checksum, not an XTC
    Published p = r.publish("X");
    std::string notXtc = "NOPE" + p.bytes.substr(4);
    p.sha = fake::sha256Hex(notXtc);
    r.http.routes[BOOK_ROUTE].body = notXtc;
    std::string& meta = r.http.routes[META_ROUTE].body;
    const size_t at = meta.find("\"sha256\":\"") + 10;
    meta.replace(at, 64, p.sha);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::VerifyFailed),
            "bytes that are not an XTC are refused");
    expectClean(r, "magic");
  }
  {
    Rig r;  // XTC magic but the parser rejects it
    r.publish("X");
    r.files.unparsable.insert(INCOMING);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::VerifyFailed),
            "a book the parser cannot load is refused");
    expectClean(r, "parser");
    check(r.library.entries().empty(), "nothing was filed");
  }
  {
    Rig r;  // the final rename fails
    const Published p = r.publish("X");
    r.files.failReplaceDest.insert(p.path);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::StorageFailed), "the move into place fails");
    expectClean(r, "rename");
    check(r.library.entries().empty(), "nothing was filed");
  }
  {
    // The index cannot be written: the old version stays, the new file goes, nothing is half-done.
    Rig r;
    const Published p = r.publish("Example", 5000);
    method::Entry old;
    old.title = "Example";
    old.path = "/Lamina/Method/Example 11111111.xtc";
    old.sha256 = std::string(64, '1');
    old.pageCount = 5;
    r.library.add(old);
    r.files.put(old.path, std::string("XTC\0", 4) + "old");
    r.library.save(r.files);
    const std::string indexBefore = r.files.files[method::INDEX_PATH];
    r.files.failReplaceDest.insert(method::INDEX_PATH);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::StorageFailed), "an index write error");
    expectClean(r, "index write");
    check(r.files.has(old.path), "the old version is still on the card");
    check(!r.files.has(p.path), "the new file was taken back off the card");
    checkEq(r.library.entries().size(), size_t{1}, "the shelf is as it was");
    checkEq(r.library.entries()[0].path, old.path, "still the old version");
    checkEq(r.files.files[method::INDEX_PATH], indexBefore, "the index on the card is untouched");
  }
}

void testReplaceAndPruneThroughTheCheck() {
  {
    // Same title: the old file goes, but only after the index is written.
    Rig r;
    const Published p = r.publish("Example", 5000);
    method::Entry old;
    old.title = "Example";
    old.path = "/Lamina/Method/Example 11111111.xtc";
    old.sha256 = std::string(64, '1');
    old.pageCount = 5;
    old.opened = true;
    r.library.add(old);
    method::Entry other;
    other.title = "Other title";
    other.path = "/Lamina/Method/Other title 22222222.xtc";
    other.sha256 = std::string(64, '2');
    other.pageCount = 5;
    other.receivedAt = 5;
    r.library.add(other);
    r.files.put(old.path, "XTC-old");
    r.files.put(other.path, "XTC-other");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "new version filed");
    check(!r.files.has(old.path), "the old version is deleted");
    check(r.files.has(other.path), "a different title is kept");
    checkEq(r.library.entries().size(), size_t{2}, "two methods");
    checkEq(r.library.entries()[0].path, p.path, "the new version is on top");
    check(!r.library.entries()[0].opened, "and it is new");
    const size_t replace = r.files.indexOfLog("replace /Lamina/Method/index.json.tmp /Lamina/Method/index.json");
    const size_t del = r.files.indexOfLog("removeBook " + old.path);
    check(replace != SIZE_MAX && del != SIZE_MAX && replace < del, "old file deleted after the index write");
  }
  {
    // Twelve methods already: the oldest is pruned after the new index is written.
    Rig r;
    const Published p = r.publish("Thirteenth", 5000);
    for (int i = 0; i < 12; ++i) {
      method::Entry e;
      e.title = "M" + std::to_string(i);
      e.path = "/Lamina/Method/M" + std::to_string(i) + " 11111111.xtc";
      e.sha256 = std::string(64, static_cast<char>('a' + i));
      e.pageCount = 5;
      e.receivedAt = 100 + i;
      r.library.add(e);
      r.files.put(e.path, "XTC-m");
    }
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "thirteenth filed");
    checkEq(r.library.entries().size(), size_t{12}, "still twelve");
    check(!r.files.has("/Lamina/Method/M0 11111111.xtc"), "the oldest file is pruned");
    check(r.files.has("/Lamina/Method/M1 11111111.xtc") && r.files.has(p.path), "the others stay");
  }
}

// ---- the list contract: several books, the mark, the cap ---------------------------------

// Runs one check on r and returns the (index, total) pairs seen while downloading.
std::vector<std::pair<uint32_t, uint32_t>> runWatching(Rig& r) {
  r.make();
  r.startedAt = r.clock.ms;
  r.check->start();
  std::vector<std::pair<uint32_t, uint32_t>> seen;
  for (int i = 0; i < 200000 && r.check->running(); ++i) {
    r.check->step();
    r.clock.ms += 10;
    if (r.check->running() && r.check->phase() == Phase::Download) {
      const auto now = std::make_pair(r.check->downloadIndex(), r.check->downloadTotal());
      if (seen.empty() || seen.back() != now) seen.push_back(now);
    }
  }
  return seen;
}

std::vector<std::string> titlesOf(const method::Library& library) {
  std::vector<std::string> out;
  for (const auto& e : library.entries()) out.push_back(e.title);
  return out;
}

void testSeveralNewBooks() {
  Rig r;
  r.publish("Seeding sheet", 3000, "b11", 2, "table");
  r.publish("Protocol", 4000, "b10", 8, "method");
  r.publish("Plate map", 2500, "b12", 1, "plate");
  const auto seen = runWatching(r);
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "three books filed");
  expectClean(r, "three new");
  checkEq(r.check->filedCount(), 3u, "three filed");
  checkEq(r.library.entries().size(), size_t{3}, "three on the shelf");
  checkEq(r.library.mark(), 12u, "the mark is the newest id");
  checkEq(r.http.opened,
          (std::vector<std::string>{META_ROUTE, Rig::route("b10"), Rig::route("b11"), Rig::route("b12")}),
          "downloaded oldest first");
  checkEq(titlesOf(r.library), (std::vector<std::string>{"Plate map", "Seeding sheet", "Protocol"}),
          "the newest id sits on top of the shelf");
  check(r.library.entries()[1].kind == method::Kind::Table && r.library.entries()[0].kind == method::Kind::Plate &&
            r.library.entries()[2].kind == method::Kind::Method,
        "each book keeps its kind");
  checkEq(seen, (std::vector<std::pair<uint32_t, uint32_t>>{{1, 3}, {2, 3}, {3, 3}}),
          "progress counts downloads: 1 of 3, 2 of 3, 3 of 3");
  checkEq(r.check->addedPath(), r.mailbox[2].path, "the check reports the newest it filed");
  check(!r.check->moreWaiting(), "nothing left waiting");
  // The shelf on the card matches.
  method::Library again;
  again.load(r.files);
  checkEq(again.serialise(), r.library.serialise(), "the index on the card matches");
}

void testDownloadCap() {
  Rig r;
  for (int i = 1; i <= 5; ++i) r.publish("Book " + std::to_string(i), 1500, "b" + std::to_string(20 + i));
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the first three are filed");
  checkEq(r.check->filedCount(), 3u, "at most three downloads per check");
  check(r.check->moreWaiting(), "the rest wait for the next check");
  checkEq(r.library.mark(), 23u, "the mark stops at the third book");
  checkEq(r.http.opened.size(), size_t{4}, "one list request and three downloads");
  checkEq(r.http.opened[1], Rig::route("b21"), "oldest first");
  expectClean(r, "capped");

  // The next check takes the other two.
  r.http.opened.clear();
  r.run();
  checkEq(r.check->filedCount(), 2u, "the next check files the remaining two");
  checkEq(r.library.mark(), 25u, "the mark reaches the newest id");
  check(!r.check->moreWaiting(), "nothing waits now");
  checkEq(r.library.entries().size(), size_t{5}, "all five are on the shelf");
  expectClean(r, "second run");

  // Books the card already holds do not use up the cap.
  Rig held;
  for (int i = 1; i <= 5; ++i) {
    const Published p = held.publish("Held " + std::to_string(i), 1500, "b" + std::to_string(40 + i));
    if (i <= 3) {
      method::Entry e;
      e.title = p.title;
      e.path = p.path;
      e.sha256 = p.sha;
      e.pageCount = 4;
      held.library.add(e);
      held.files.put(p.path, p.bytes);
    }
  }
  held.run();
  checkEq(held.check->filedCount(), 2u, "the two missing books are downloaded in the same check");
  checkEq(held.library.mark(), 45u, "and the mark passes the three that were already there");
  check(!held.check->moreWaiting(), "nothing left waiting");
}

void testShaAlreadyOnTheCard() {
  // A re-sent book has a new id but the same bytes: no download, the mark moves, NEW and progress stay.
  Rig r;
  const Published p = r.publish("Protocol", 4000, "b13");
  method::Entry e;
  e.title = p.title;
  e.path = p.path;
  e.sha256 = p.sha;
  e.bookId = "b9";
  e.pageCount = 8;
  e.opened = true;
  e.receivedAt = 555;
  r.library.add(e);
  r.library.advanceMark(9);
  r.files.put(p.path, p.bytes);
  r.files.progress[p.path] = 3;
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate), "nothing new");
  checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "no download");
  checkEq(r.library.mark(), 13u, "the mark advanced past the re-send");
  const auto* kept = r.library.find(p.path);
  check(kept && kept->opened && kept->receivedAt == 555 && kept->bookId == "b9", "the entry is untouched");
  checkEq(r.files.progress[p.path], int64_t{3}, "and so is the reading position");
  check(r.files.has(method::INDEX_PATH) && r.files.files[method::INDEX_PATH] == r.library.serialise(),
        "the new mark is written");
  expectClean(r, "sha present");

  // Mixed: one already here (id 11), one new (id 12).
  Rig m;
  const Published here = m.publish("Here", 3000, "b11");
  const Published fresh = m.publish("Fresh", 3000, "b12");
  method::Entry h;
  h.title = here.title;
  h.path = here.path;
  h.sha256 = here.sha;
  h.pageCount = 4;
  m.library.add(h);
  m.files.put(here.path, here.bytes);
  m.run();
  checkEq(static_cast<int>(m.check->result()), static_cast<int>(Result::NewMethod), "the new one is filed");
  checkEq(m.http.opened, (std::vector<std::string>{META_ROUTE, Rig::route("b12")}),
          "only the missing book is downloaded");
  checkEq(m.library.mark(), 12u, "mark at the newest");
  check(m.library.find(fresh.path) != nullptr, "fresh book on the shelf");
}

void testMarkOnFailures() {
  {
    // A book that fails verification is skipped and passed over, so it is not retried forever.
    Rig r;
    const Published bad = r.publish("Tampered", 4000, "b11");
    std::string tampered = bad.bytes;
    tampered[100] = tampered[100] == 'z' ? 'y' : 'z';
    r.http.routes[Rig::route("b11")].body = tampered;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::VerifyFailed),
            "the status says it did not verify");
    checkEq(r.library.mark(), 11u, "the mark moved past it");
    check(r.library.entries().empty() && !r.files.has(INCOMING), "nothing kept");
    check(r.files.has(method::INDEX_PATH), "the mark was written");
    expectClean(r, "verify skip");
    r.http.opened.clear();
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate), "the next check leaves it alone");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "and does not download it again");
  }
  {
    // One bad book among good ones: the good ones are filed, the run reports success.
    Rig r;
    r.publish("Good one", 3000, "b10");
    const Published bad = r.publish("Bad one", 3000, "b11");
    r.publish("Good two", 3000, "b12");
    std::string tampered = bad.bytes;
    tampered[50] = tampered[50] == 'z' ? 'y' : 'z';
    r.http.routes[Rig::route("b11")].body = tampered;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the good books are filed");
    checkEq(r.check->filedCount(), 2u, "two filed");
    checkEq(r.library.mark(), 12u, "the mark is past all three");
    expectClean(r, "one bad of three");
  }
  {
    // A network failure stops the run and does not advance past the book.
    Rig r;
    r.publish("Stalls", 6000, "b11");
    r.http.routes[Rig::route("b11")].stallAt = 2800;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "the download stopped partway");
    checkEq(r.library.mark(), 0u, "the mark did not move");
    check(r.check->filedCount() == 0 && r.library.entries().empty(), "nothing filed");
    expectClean(r, "network failure");
    r.http.routes[Rig::route("b11")].stallAt = SIZE_MAX;
    r.http.opened.clear();
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the next check retries it");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE, Rig::route("b11")}), "by requesting the same book");
    checkEq(r.library.mark(), 11u, "and now the mark moves");
  }
  {
    // A failure on the second of three keeps the first and stops.
    Rig r;
    r.publish("One", 3000, "b10");
    r.publish("Two", 6000, "b11");
    r.publish("Three", 3000, "b12");
    r.http.routes[Rig::route("b11")].stallAt = 2000;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "the run stops at the failure");
    checkEq(r.check->filedCount(), 1u, "the first book was kept");
    checkEq(r.library.mark(), 10u, "the mark is at the last book dealt with");
    checkEq(r.http.opened.size(), size_t{3}, "the third was not attempted");
    check(r.library.entries().size() == 1 && !r.files.has(INCOMING), "no partial file, one book");
    expectClean(r, "failure on the second");
  }
  {
    // A refused token or a dead server mid-run stops it the same way.
    Rig r;
    r.publish("One", 3000, "b10");
    r.publish("Two", 3000, "b11");
    r.http.routes[Rig::route("b11")].open = method::HttpOpen::Unauthorized;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::AuthRefused),
            "token refused on the second book");
    checkEq(r.library.mark(), 10u, "mark after the first only");
    expectClean(r, "auth mid-run");
  }
  {
    // Low space on the second book stops the run, keeps the first, and leaves the rest for later.
    Rig r;
    r.publish("One", 3000, "b10");
    const Published two = r.publish("Two", 900000, "b11");
    r.files.free = 400000 + 256 * 1024;  // fits the first book and not the second
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::LowSpace), "no room for the second");
    checkEq(r.check->neededBytes(), static_cast<uint64_t>(two.bytes.size() + 256 * 1024), "reports what it needs");
    checkEq(r.check->filedCount(), 1u, "the first is kept");
    checkEq(r.library.mark(), 10u, "mark after the first only");
    check(r.check->moreWaiting(), "the second waits");
    expectClean(r, "low space mid-run");
  }
}

void testGoneRereadsOnce() {
  {
    // The book was replaced after the list was read: read the list again once and take the new one.
    Rig r;
    r.publish("Protocol", 3000, "b11");
    r.http.routes[Rig::route("b11")].open = method::HttpOpen::Gone;
    r.http.onOpen = [&r](const std::string& path) {
      if (path != Rig::route("b11")) return;
      r.publish("Protocol", 3500, "b13");  // the Worker replaced it
    };
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the replacement is filed");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE, Rig::route("b11"), META_ROUTE, Rig::route("b13")}),
            "list, 410, list again, the new book");
    checkEq(r.library.mark(), 13u, "mark at the replacement");
    checkEq(r.library.entries().size(), size_t{1}, "one book");
    checkEq(r.check->downloadTotal(), 1u, "a 410 does not count as a download");
    expectClean(r, "410 reread");
  }
  {
    // A second 410 in the same check is a failed download, not a loop.
    Rig r;
    r.publish("Protocol", 3000, "b11");
    r.http.routes[Rig::route("b11")].open = method::HttpOpen::Gone;
    r.http.onOpen = [&r](const std::string& path) {
      if (path != Rig::route("b11")) return;
      r.publish("Protocol", 3500, "b13");
      r.http.routes[Rig::route("b13")].open = method::HttpOpen::Gone;
    };
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "a second 410 fails the download");
    checkEq(r.http.opened.size(), size_t{4}, "the list was read only twice");
    checkEq(r.library.mark(), 0u, "mark unchanged");
    expectClean(r, "two 410");
  }
  {
    // The replaced book is gone from the list afterwards: the check simply ends up to date.
    Rig r;
    r.publish("Gone", 3000, "b11");
    r.http.routes[Rig::route("b11")].open = method::HttpOpen::Gone;
    r.http.onOpen = [&r](const std::string& path) {
      if (path == Rig::route("b11")) r.http.routes[META_ROUTE].body = "{\"books\":[]}";
    };
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate),
            "nothing left to fetch after the reread");
    expectClean(r, "410 then empty");
  }
}

void testMailboxReset() {
  {
    // The mailbox started over: its newest id is below the mark.
    Rig r;
    r.library.advanceMark(50);
    const Published p = r.publish("After reset", 3000, "b3");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a book below the mark is fetched after a reset");
    checkEq(r.library.mark(), 3u, "the mark now follows the new mailbox");
    check(r.library.find(p.path) != nullptr, "filed");
    expectClean(r, "reset");
  }
  {
    // After a reset, books the card already holds are skipped by SHA-256.
    Rig r;
    r.library.advanceMark(50);
    const Published have = r.publish("Have it", 3000, "b2");
    const Published fresh = r.publish("Fresh one", 3000, "b3");
    method::Entry e;
    e.title = have.title;
    e.path = have.path;
    e.sha256 = have.sha;
    e.pageCount = 4;
    r.library.add(e);
    r.files.put(have.path, have.bytes);
    r.run();
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE, Rig::route("b3")}),
            "only the book the card lacks is fetched");
    checkEq(r.library.mark(), 3u, "mark follows the new ids");
    check(r.library.find(fresh.path) != nullptr && r.library.find(have.path) != nullptr, "both on the shelf");
  }
  {
    // An empty mailbox changes nothing, whatever the mark.
    Rig r;
    r.library.advanceMark(50);
    r.http.routes[META_ROUTE].body = "{\"books\":[]}";
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate), "empty list");
    checkEq(r.library.mark(), 50u, "the mark is kept");
    check(!r.files.has(method::INDEX_PATH), "and nothing is written");
  }
  {
    // A list of only damaged entries must not look like a reset.
    Rig r;
    r.library.advanceMark(50);
    r.publish("Bad", 3000, "b3", 8, "sheet");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UnreadableList),
            "nothing usable, and the status says so");
    checkEq(r.library.mark(), 50u, "the mark is kept");
  }
}

void testReplacementAcrossKinds() {
  Rig r;
  const Published old = r.publish("Seeding sheet", 3000, "b11", 2, "table");
  r.run();
  checkEq(r.library.entries().size(), size_t{1}, "the sheet is on the shelf");
  const std::string oldPath = old.path;
  // The Mac sends it again with new numbers: a new id, a new hash, the same title.
  r.publish("Seeding sheet", 3300, "b12", 2, "table");
  r.run();
  checkEq(r.library.entries().size(), size_t{1}, "a re-sent sheet replaces its previous version");
  check(!r.files.has(oldPath), "the old file is deleted");
  check(r.library.entries()[0].kind == method::Kind::Table && !r.library.entries()[0].opened, "the new version is new");
  const size_t index = r.files.indexOfLog("replace /Lamina/Method/index.json.tmp /Lamina/Method/index.json");
  const size_t del = r.files.indexOfLog("removeBook " + oldPath);
  check(index != SIZE_MAX && del != SIZE_MAX && index < del, "after the index write");

  // The 12-book prune applies to every kind.
  Rig full;
  for (int i = 0; i < 12; ++i) {
    method::Entry e;
    e.title = "Existing " + std::to_string(i);
    e.path = "/Lamina/Method/Existing " + std::to_string(i) + " 11111111.xtc";
    e.sha256 = std::string(64, static_cast<char>('a' + i));
    e.pageCount = 3;
    e.kind = i % 2 ? method::Kind::Table : method::Kind::Method;
    e.receivedAt = 100 + i;
    full.library.add(e);
    full.files.put(e.path, "XTC-x");
  }
  full.publish("Newest board", 2000, "b30", 2, "board");
  full.run();
  checkEq(full.library.entries().size(), size_t{12}, "still twelve");
  check(!full.files.has("/Lamina/Method/Existing 0 11111111.xtc"), "the oldest of any kind is pruned");
  check(full.library.entries()[0].kind == method::Kind::Board, "the board is on top");
}

void testClockIsRefreshedDuringALongRun() {
  // Three slow downloads outlast the 90 s clock window: the check syncs again instead of failing.
  Rig r;
  r.publish("One", 6000, "b10");
  r.publish("Two", 6000, "b11");
  r.publish("Three", 6000, "b12");
  for (const char* id : {"b10", "b11", "b12"}) r.http.routes[Rig::route(id)].idleBefore = 4500;  // 45 s of silence each
  r.limits.stallMs = 60000;  // the server is slow, not stalled
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "all three are filed");
  checkEq(r.check->filedCount(), 3u, "three filed");
  check(r.clock.beginCalls >= 2, "the clock was synced again when the window ran out");
  expectClean(r, "long run");

  // A refresh that fails ends the run like the Clock phase does.
  Rig f;
  f.publish("One", 6000, "b10");
  f.publish("Two", 6000, "b11");
  for (const char* id : {"b10", "b11"})
    f.http.routes[Rig::route(id)].idleBefore = 9500;  // 95 s each, longer than the clock window
  f.limits.stallMs = 120000;
  f.limits.downloadMs = 400000;
  f.make();
  f.check->start();
  bool cut = false;
  for (int i = 0; i < 200000 && f.check->running(); ++i) {
    f.check->step();
    f.clock.ms += 10;
    if (!cut && f.check->filedCount() == 1) {
      f.clock.syncNever = true;  // the NTP server goes away
      cut = true;
    }
  }
  checkEq(static_cast<int>(f.check->result()), static_cast<int>(Result::ServerError), "a failed refresh fails the run");
  checkEq(f.check->filedCount(), 1u, "the first book stays");
  expectClean(f, "refresh failed");

  // The same refresh, with the retained clock trusted: it stands in for the sync that never answers, and the run goes
  // on.
  Rig s;
  s.publish("One", 6000, "b10");
  s.publish("Two", 6000, "b11");
  for (const char* id : {"b10", "b11"}) s.http.routes[Rig::route(id)].idleBefore = 9500;
  s.limits.stallMs = 120000;
  s.limits.downloadMs = 400000;
  s.clock.retainedTrusted = true;
  s.make();
  s.check->start();
  bool cutAgain = false;
  for (int i = 0; i < 200000 && s.check->running(); ++i) {
    s.check->step();
    s.clock.ms += 10;
    if (!cutAgain && s.check->filedCount() == 1) {
      s.clock.syncNever = true;
      cutAgain = true;
    }
  }
  checkEq(static_cast<int>(s.check->result()), static_cast<int>(Result::NewMethod),
          "a refresh that fails falls back on a trusted retained clock");
  checkEq(s.check->filedCount(), 2u, "and the second book is filed");
  checkEq(s.clock.retainedCalls, 1, "the retained clock was asked once");
  expectClean(s, "refresh on the retained clock");
}

void testCancelBetweenBooks() {
  Rig r;
  r.publish("One", 3000, "b10");
  r.publish("Two", 40000, "b11");
  r.http.routes[Rig::route("b11")].slice = 500;
  r.make();
  r.startedAt = r.clock.ms;
  r.check->start();
  int downloading = 0;
  for (int i = 0; i < 200000 && r.check->running(); ++i) {
    r.check->step();
    r.clock.ms += 10;
    if (r.check->phase() == Phase::Download && r.check->downloadIndex() == 2 && ++downloading == 15) break;
  }
  check(r.check->running() && r.files.has(INCOMING), "mid-way through the second download");
  r.check->cancel();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Cancelled), "cancelled");
  expectClean(r, "cancel in the second download");
  checkEq(r.library.entries().size(), size_t{1}, "the first book stays filed");
  checkEq(r.library.mark(), 10u, "and the mark with it");
}

void testCancelEverywhere() {
  using Network = fake::FakeWifi::Network;
  struct Case {
    const char* name;
    std::function<void(Rig&)> setup;
    Phase phase;
    int extra;
  };
  const std::vector<Case> cases = {
      {"during the last-network join",
       [](Rig& r) {
         r.publish("X");
         r.wifi.saved = {Network{60000, false, true, -50}};
       },
       Phase::Wifi, 20},
      {"during the Wi-Fi scan",
       [](Rig& r) {
         r.publish("X");
         r.wifi.saved = {Network{60000, false, true, -50}};
         r.wifi.last = -1;
       },
       Phase::Wifi, 20},
      {"during a scanned network join",
       [](Rig& r) {
         r.publish("X");
         r.wifi.saved = {Network{60000, false, true, -50}};
         r.wifi.last = -1;
         r.wifi.scanTakesMs = 100;
       },
       Phase::Wifi, 60},
      {"during the clock sync",
       [](Rig& r) {
         r.publish("X");
         r.clock.syncNever = true;
       },
       Phase::Clock, 20},
      {"while waiting for the render task",
       [](Rig& r) {
         r.publish("X");
         r.http.readyWaits = -1;
       },
       Phase::Mailbox, 20},
      {"before the mailbox is asked", [](Rig& r) { r.publish("X"); }, Phase::Mailbox, 0},
      {"while the mailbox replies",
       [](Rig& r) {
         r.publish("X");
         r.http.routes[META_ROUTE].slice = 10;
       },
       Phase::Mailbox, 3},
      {"at the start of the download", [](Rig& r) { r.publish("X"); }, Phase::Download, 0},
      {"in the middle of the download",
       [](Rig& r) {
         r.publish("X", 40000);
         r.http.routes[BOOK_ROUTE].slice = 500;
       },
       Phase::Download, 20},
      {"while verifying", [](Rig& r) { r.publish("X"); }, Phase::Verify, 0},
      {"between the checksum and filing", [](Rig& r) { r.publish("X"); }, Phase::Verify, 2},
  };
  for (const Case& c : cases) {
    Rig r;
    c.setup(r);
    r.runUntil(c.phase, c.extra);
    const std::string n = std::string("cancel ") + c.name;
    check(r.check->running(), (n + ": still running before the cancel").c_str());
    const std::string indexBefore = r.library.serialise();
    r.check->cancel();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::Cancelled),
            (n + ": result is Cancelled").c_str());
    expectClean(r, n.c_str());
    check(r.library.serialise() == indexBefore && r.library.entries().empty(), (n + ": nothing was filed").c_str());
    const int offs = r.wifi.offCalls;
    r.check->cancel();
    r.check->step();
    checkEq(r.wifi.offCalls, offs, (n + ": a second cancel does nothing").c_str());
  }

  // Destroying a running check cleans up the same way.
  Rig r;
  r.publish("X", 40000);
  r.http.routes[BOOK_ROUTE].slice = 500;
  r.runUntil(Phase::Download, 20);
  check(r.check->running() && r.files.has(INCOMING) && r.wifi.radioOn,
        "mid-download there is a partial file and a radio");
  r.check.reset();
  check(!r.files.has(INCOMING) && !r.wifi.radioOn && !r.http.isOpen,
        "destroying the check removes the partial file and the radio");
}

void testPhaseTimeouts() {
  {
    // The verify phase is bounded too: a clock that jumps ends it.
    Rig r;
    r.publish("X");
    r.runUntil(Phase::Verify, 0);
    r.clock.ms += r.limits.verifyMs + 1;
    r.check->step();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::VerifyFailed), "verify has a hard limit");
    expectClean(r, "verify timeout");
  }
  {
    // A slow single step (the TLS handshake blocks up to 5 s) then trips the phase limit on the next step.
    Rig r;
    r.publish("X");
    r.runUntil(Phase::Mailbox, 0);
    r.clock.ms += r.limits.mailboxMs + 1;
    r.check->step();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError), "mailbox has a hard limit");
    expectClean(r, "mailbox limit");
  }
}

void testRestart() {
  Rig r;
  r.publish("First");
  r.run();
  check(r.check->result() == Result::NewMethod, "first run files a method");
  r.http.routes.clear();
  r.http.routes[META_ROUTE].body = "{\"books\":[]}";
  r.check->start();
  for (int i = 0; i < 1000 && r.check->running(); ++i) {
    r.check->step();
    r.clock.ms += 10;
  }
  check(r.check->result() == Result::UpToDate, "a finished check can run again");
  check(r.check->addedPath().empty(), "and forgets what the last run filed");
}

void testObserverOutcomes() {
  Rig r;
  r.publish("X");
  r.wifi.saved = {fake::FakeWifi::Network{600000, false, true, -50}};
  r.run();
  bool logged = false;
  for (const auto& e : r.observer.events)
    logged =
        logged || (e.rfind("leave wifi ", 0) == 0 && e.size() > 11 && e.compare(e.size() - 11, 11, "wifi failed") == 0);
  check(logged, "a failed phase logs its duration and outcome");
}

}  // namespace

// ---- a timer wake's check (Stage B) --------------------------------------------------------
//
// The note check has just run: Wi-Fi is up and the clock was synced a moment ago. The check starts with what is left
// of the wake budget and the wake rules on. Nothing here has a panel: the check has no display port to draw on.

namespace wake = method::wake;

struct WakeRig : Rig {
  explicit WakeRig(uint32_t budgetMs = wake::BUDGET_MS - 15000, const std::string& protectedPath = "",
                   uint32_t blockedBookId = 0) {
    joinedByTheNoteCheck();
    limits.wake = wake::rulesFor(budgetMs, protectedPath, blockedBookId);
    limits.maxDownloads = limits.wake.maxBooks;
  }
  void joinedByTheNoteCheck() {
    wifi.joined = true;
    wifi.radioOn = true;
    clock.ready = true;
    clock.syncedAt = clock.ms;
  }
  wake::Verdict verdict(size_t i) const { return check->decisions().at(i).verdict; }
  size_t decided() const { return check->decisions().size(); }
};

// A book the reader downloaded (it has a SHA-256) and the owner has opened. `hex` picks the digit its digest is made
// of.
method::Entry downloadedEntry(const std::string& title, int64_t receivedAt, int hex) {
  method::Entry e;
  e.title = title;
  e.sha256 = std::string(64, "0123456789abcdef"[hex & 15]);
  e.path = bookPathFor(title, e.sha256);
  e.pageCount = 5;
  e.receivedAt = receivedAt;
  e.opened = true;
  return e;
}

void testWakeFilesABookInTheSameSession() {
  WakeRig r;
  const Published p = r.publish("Example column clean-up", 12000);
  r.run();
  checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "the wake files the book");
  expectClean(r, "wake books");
  checkEq(r.check->filedCount(), 1u, "one book filed");
  check(r.wifi.log.empty(), "no Wi-Fi join: the note check's session is used");
  checkEq(r.clock.beginCalls, 0, "no clock sync: the note check's window is used");
  checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE, BOOK_ROUTE}), "the list, then the book");
  check(r.files.has(p.path) && r.files.files[p.path] == p.bytes, "the book is on the card, byte for byte");
  checkEq(r.library.mark(), 12u, "the high-water mark moved to the book's id");
  check(!r.library.entries().empty() && !r.library.entries()[0].opened, "the shelf shows it NEW next time");
  check(r.files.has(method::INDEX_PATH) && r.files.files[method::INDEX_PATH] == r.library.serialise(),
        "the index on the card is the library");
  checkEq(r.decided(), size_t{1}, "one decision is recorded");
  check(r.verdict(0) == wake::Verdict::Download, "and it is to download");
  checkEq(r.check->decisions()[0].bookId, 12u, "for book 12");
  checkEq(r.check->decisions()[0].bytes, 12000u, "of its announced length");
  check(r.check->decisions()[0].remainingMs <= wake::BUDGET_MS - 15000, "with the remaining time reported");
  checkEq(r.check->failedBookId(), 0u, "nothing failed");
  // The events, in order: the join and the clock sync are skipped at once.
  const auto& ev = r.observer.events;
  check(ev.size() > 4 && ev[0] == "enter wifi" && ev[1].find("leave wifi 0ms") == 0, "the Wi-Fi phase ends at once");
  check(ev[2] == "enter clock" && ev[3].find("leave clock 0ms") == 0, "and so does the clock phase");
  check(ev[4] == "enter mailbox", "the first work is the mailbox");
}

void testWakeFilesTwoThenStops() {
  WakeRig r;
  r.publish("Book 1", 1500, "b21");
  r.publish("Book 2", 1500, "b22");
  r.publish("Book 3", 1500, "b23");
  r.run();
  expectClean(r, "wake cap");
  checkEq(r.check->filedCount(), 2u, "two books per wake");
  check(r.check->moreWaiting(), "the third waits");
  checkEq(r.library.mark(), 22u, "the mark stops at the second");
  checkEq(r.decided(), size_t{3}, "three decisions");
  check(r.verdict(0) == wake::Verdict::Download && r.verdict(1) == wake::Verdict::Download, "two to download");
  check(r.verdict(2) == wake::Verdict::Limit, "then the count");
  checkEq(r.http.opened.size(), size_t{3}, "the list and two books");

  // The next wake takes the third.
  r.http.opened.clear();
  r.joinedByTheNoteCheck();
  r.run();
  checkEq(r.check->filedCount(), 1u, "the next wake files the third");
  checkEq(r.library.mark(), 23u, "the mark reaches the newest");
  check(!r.check->moreWaiting(), "nothing waits now");
  expectClean(r, "second wake");
}

void testWakeBatteryFloor() {
  {
    WakeRig r;
    r.publish("Protocol", 4000);
    r.battery.level = 29;
    r.run();
    expectClean(r, "29 %");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "29 %: only the list is read, nothing downloaded");
    checkEq(r.check->filedCount(), 0u, "29 %: nothing filed");
    checkEq(r.library.mark(), 0u, "29 %: the mark does not move");
    check(r.check->moreWaiting(), "29 %: the book waits for the shelf");
    checkEq(r.decided(), size_t{1}, "29 %: one decision");
    check(r.verdict(0) == wake::Verdict::Battery, "29 %: it says the battery");
    checkEq(r.check->decisions()[0].battery, 29, "29 %: with the reading");
  }
  {
    WakeRig r;
    r.publish("Protocol", 4000);
    r.battery.level = 30;
    r.run();
    checkEq(r.check->filedCount(), 1u, "30 %: filed");
  }
  {
    // The gauge is read again for every book: it fell during the first download.
    WakeRig r;
    r.publish("First", 3000, "b21");
    r.publish("Second", 3000, "b22");
    r.http.onOpen = [&](const std::string& path) {
      if (path == Rig::route("b21")) r.battery.level = 20;
    };
    r.run();
    expectClean(r, "battery fell");
    checkEq(r.check->filedCount(), 1u, "the first book is filed");
    checkEq(r.library.mark(), 21u, "the mark stops at it");
    check(r.verdict(1) == wake::Verdict::Battery, "the second is left: the battery fell");
  }
  {
    // The shelf has no such floor beyond its own 15 %, and no size cap.
    Rig r;
    check(!r.limits.wake.active, "the shelf's limits have no wake rules");
    r.publish("Big", 1200000);
    r.battery.level = 20;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "the shelf files a 1.2 MB book at 20 %");
    check(r.check->decisions().empty(), "and records no wake decisions");
  }
}

void testWakeSizeCap() {
  {
    WakeRig r(200000);  // the cap alone: at 12 KB/s a 1 MB book needs 94 s, more than a wake has
    r.publish("Exactly 1 MB", 1000000);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "1 MB is filed");
    expectClean(r, "1 MB");
  }
  {
    WakeRig r;  // 75 s left, and the whole of 1 MB is planned at 94.3 s: the time says no, not the size
    r.publish("Exactly 1 MB, short of time", 1000000);
    r.run();
    expectClean(r, "1 MB, short of time");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "1 MB in 75 s: not requested");
    check(r.verdict(0) == wake::Verdict::NoTime, "1 MB in 75 s: no time");
    checkEq(r.check->decisions()[0].neededMs, 94334u, "planned at 94.3 s");
  }
  {
    WakeRig r;
    r.publish("Over 1 MB", 1000001);
    r.run();
    expectClean(r, "over 1 MB");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "over 1 MB: not requested");
    check(r.verdict(0) == wake::Verdict::TooLarge, "over 1 MB: too large");
    checkEq(r.library.mark(), 0u, "over 1 MB: the mark does not move");
    check(r.check->moreWaiting() && r.check->filedCount() == 0, "over 1 MB: left for the shelf");
    check(!r.files.has(INCOMING), "over 1 MB: no partial file");
  }
  {
    // The declined book is at the head: a newer small book behind it must NOT be filed, or the mark would leave
    // the big one behind it for good.
    WakeRig r;
    r.publish("Big", 1200000, "b13");
    r.publish("Small", 4000, "b14");
    r.run();
    expectClean(r, "big at the head");
    checkEq(r.check->filedCount(), 0u, "big at the head: nothing is filed");
    checkEq(r.library.mark(), 0u, "big at the head: the mark does not move");
    check(r.check->moreWaiting(), "big at the head: both wait");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "big at the head: no book is requested");
    checkEq(r.decided(), size_t{1}, "big at the head: the run stopped at the first");

    // The shelf's next check takes both, oldest first.
    Rig shelf;
    shelf.http.routes = r.http.routes;
    shelf.files = r.files;
    shelf.library = r.library;
    shelf.run();
    checkEq(shelf.check->filedCount(), 2u, "the shelf files what the wake left");
    checkEq(shelf.library.mark(), 14u, "and the mark passes both");
  }
  {
    // The other order: the small book is older, so it goes first and the big one waits behind it.
    WakeRig r;
    r.publish("Small", 4000, "b13");
    r.publish("Big", 1200000, "b14");
    r.run();
    checkEq(r.check->filedCount(), 1u, "small first: it is filed");
    checkEq(r.library.mark(), 13u, "the mark stops before the big one");
    check(r.check->moreWaiting() && r.verdict(1) == wake::Verdict::TooLarge, "and the big one is left");
  }
}

void testWakeBudget() {
  // What the check has used by the time it decides about a 240 KB book (31 s planned).
  uint32_t elapsedAtDecision = 0;
  {
    WakeRig r(100000);
    r.publish("Quarter megabyte", 240000);
    r.run();
    checkEq(r.check->filedCount(), 1u, "a roomy budget files it");
    elapsedAtDecision = 100000 - r.check->decisions()[0].remainingMs;
    checkEq(r.check->decisions()[0].neededMs, 31000u, "it is planned at 31 s");
    check(elapsedAtDecision > 0 && elapsedAtDecision < 5000, "and decided about within a few seconds");
  }
  {
    WakeRig r(elapsedAtDecision + 31000);
    r.publish("Quarter megabyte", 240000);
    r.run();
    expectClean(r, "exact budget");
    checkEq(r.check->filedCount(), 1u, "a budget that fits to the millisecond files it");
    check(r.verdict(0) == wake::Verdict::Download, "exact: download");
  }
  {
    WakeRig r(elapsedAtDecision + 31000 - 1);
    r.publish("Quarter megabyte", 240000);
    r.run();
    expectClean(r, "one short");
    checkEq(r.check->filedCount(), 0u, "a millisecond short does not");
    check(r.verdict(0) == wake::Verdict::NoTime, "one short: no time");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "one short: not requested");
    checkEq(r.library.mark(), 0u, "one short: the mark does not move");
  }
  {
    // A link slower than the plan: the download is abandoned when the budget ends, cleanly.
    WakeRig r(20000);
    r.publish("Slow", 20000);              // planned 8 s + 1.7 s + 3 s = 12.7 s
    r.http.routes[BOOK_ROUTE].slice = 10;  // 1 KB/s on the fake clock
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "the slow download is abandoned");
    expectClean(r, "budget spent in a download");
    check(r.elapsed() <= 20000 + 50, "at the end of the budget, not later");
    check(r.elapsed() >= 20000, "and not earlier");
    checkEq(r.check->failedBookId(), 12u, "the book is the one that failed");
    checkEq(r.library.mark(), 0u, "the mark does not move past it");
    check(r.files.count(std::string("remove ") + INCOMING) >= 1, "the partial file was removed");
  }
  {
    // Spent while waiting for the list.
    WakeRig r(3000);
    r.publish("Never listed");
    r.http.resolveNever = true;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::ServerError),
            "no answer in time is a server error");
    expectClean(r, "budget spent before the list");
    check(r.elapsed() <= 3050, "within the budget, not the 8 s lookup limit");
    checkEq(r.check->failedBookId(), 0u, "no book is blamed");
  }
  {
    // Verify is local work: a budget that ends during it still files the book that has arrived. The plan is told the
    // link is instant so that this tiny budget gets past the decision.
    const auto instant = [](WakeRig& rig) {
      rig.limits.wake.requestMs = 0;
      rig.limits.wake.filingMs = 0;
      rig.limits.wake.bytesPerSec = 1000000000;
    };
    WakeRig probe(100000);
    instant(probe);
    probe.publish("Arrives", 3000);
    probe.runUntil(Phase::Verify);
    const uint32_t atVerify = probe.elapsed();
    WakeRig r(atVerify - 5);
    instant(r);
    r.publish("Arrives", 3000);
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "a budget ending in verify still files");
    expectClean(r, "budget spent in verify");
    WakeRig early(atVerify - 25);
    instant(early);
    early.publish("Arrives", 3000);
    early.run();
    checkEq(static_cast<int>(early.check->result()), static_cast<int>(Result::DownloadFailed),
            "one ending before it does not");
    expectClean(early, "budget spent just before verify");
  }
  {
    // An absent budget is none at all: the check ends at its first step.
    WakeRig r(0);
    r.publish("Anything");
    r.run();
    expectClean(r, "no budget");
    check(!r.check->running(), "no budget: finished");
    checkEq(r.check->filedCount(), 0u, "no budget: nothing filed");
  }
}

void testWakeLeavesTheBookOnThePanel() {
  const std::string sha1(64, 'a');
  const std::string oldPath = bookPathFor("Protocol", sha1);
  {
    // A newer version of the book the button wake reopens: it waits, the old file stays.
    WakeRig r(wake::BUDGET_MS, oldPath);
    method::Entry old = downloadedEntry("Protocol", 100, 10);
    r.library.add(old);
    r.library.advanceMark(5);
    r.files.put(old.path, "XTC-old");
    r.publish("Protocol", 6000);
    r.run();
    expectClean(r, "new version of the open book");
    checkEq(r.check->filedCount(), 0u, "the new version is not filed");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "and not even downloaded");
    check(r.files.has(old.path) && r.files.files[old.path] == "XTC-old", "the book on the panel is untouched");
    check(r.library.find(old.path) != nullptr && r.library.entries().size() == 1, "and the library still lists it");
    checkEq(r.library.mark(), 5u, "the mark does not move");
    check(r.verdict(0) == wake::Verdict::Protected, "the reason is the book on the panel");
    check(r.check->moreWaiting(), "the new version waits");
    check(!r.files.has(INCOMING), "no partial file");

    // The shelf's next check (the book is no longer open) replaces it, as it always did.
    Rig shelf;
    shelf.http.routes = r.http.routes;
    shelf.files = r.files;
    shelf.library = r.library;
    shelf.run();
    checkEq(shelf.check->filedCount(), 1u, "the shelf files the new version");
    check(!shelf.files.has(old.path), "and deletes the old one");
  }
  {
    // The same new version while a different book is the open one: nothing to protect, it replaces as usual.
    WakeRig r(wake::BUDGET_MS, "/Lamina/Method/Something else 12345678.xtc");
    method::Entry old = downloadedEntry("Protocol", 100, 10);
    r.library.add(old);
    r.library.advanceMark(5);
    r.files.put(old.path, "XTC-old");
    r.publish("Protocol", 6000);
    r.run();
    checkEq(r.check->filedCount(), 1u, "another book is open: the new version is filed");
    check(!r.files.has(old.path), "and the old version is deleted");
  }
  {
    // Twelve books: a thirteenth prunes the oldest. If the oldest is the open one, the wake leaves the thirteenth.
    for (int protectedIndex : {0, 1}) {
      std::vector<std::string> paths;
      for (int i = 0; i < 12; ++i) paths.push_back(downloadedEntry("M" + std::to_string(i), 100 + i, i).path);
      WakeRig r(wake::BUDGET_MS, paths[static_cast<size_t>(protectedIndex)]);
      for (int i = 0; i < 12; ++i) {
        method::Entry e = downloadedEntry("M" + std::to_string(i), 100 + i, i);
        r.library.add(e);
        r.files.put(e.path, "XTC-m");
      }
      r.library.advanceMark(5);
      const Published p = r.publish("Thirteenth", 4000);
      r.run();
      expectClean(r, "prune");
      if (protectedIndex == 0) {
        checkEq(r.check->filedCount(), 0u, "the oldest is the open book: the thirteenth waits");
        check(r.verdict(0) == wake::Verdict::Protected, "because of the book on the panel");
        check(r.files.has(paths[0]) && r.library.entries().size() == 12, "the twelve stay");
      } else {
        checkEq(r.check->filedCount(), 1u, "the open book is not the oldest: the thirteenth is filed");
        check(r.files.has(p.path), "it is on the card");
        check(!r.files.has(paths[0]) && r.files.has(paths[1]), "the oldest is pruned, the open book stays");
      }
    }
  }
  {
    // The last guard: the library changed after the decision, so filing the book would delete the open one. The
    // file stays on the card and the book is filed.
    const std::string openPath = downloadedEntry("M0", 100, 0).path;
    WakeRig r(wake::BUDGET_MS, openPath);
    for (int i = 0; i < 11; ++i) {
      method::Entry e = downloadedEntry("M" + std::to_string(i), 100 + i, i);
      r.library.add(e);
      r.files.put(e.path, "XTC-m");
    }
    r.library.advanceMark(5);
    const Published p = r.publish("Thirteenth", 4000);
    r.http.onOpen = [&](const std::string& path) {
      if (path != BOOK_ROUTE) return;
      // One more book is filed by another route while this one downloads: the shelf is full now, so filing this
      // book would prune the oldest, which is the open one.
      method::Entry e = downloadedEntry("N20", 220, 12);
      r.library.add(e);
      r.files.put(e.path, "XTC-n");
      checkEq(r.library.entries().size(), size_t{12}, "twelve books when the download starts to arrive");
    };
    r.run();
    expectClean(r, "last guard");
    checkEq(r.check->filedCount(), 1u, "the book is filed");
    check(r.files.has(p.path), "it is on the card");
    check(r.files.has(openPath), "and the book on the panel was not deleted");
    check(r.library.find(openPath) == nullptr, "though the shelf no longer lists it");
    // The shelf is full, so the next load does not adopt it either: it stays an unindexed file on the card.
    method::Library again;
    const auto report = again.load(r.files);
    checkEq(report.adopted, size_t{0}, "a full shelf adopts nothing");
    check(again.find(openPath) == nullptr && r.files.has(openPath), "and the file is still there, unlisted");
  }
}

// What Library::load does with a file nobody indexed (the file the last guard leaves behind), as the comment in
// Check::stepVerify says: adopted into a free slot, never under a title that is already on the shelf.
void testAnUnindexedBookIsNotAlwaysAdopted() {
  const auto setup = [](Rig& r) {
    method::Entry kept = downloadedEntry("Protocol", 100, 2);  // the new version, filed
    r.library.add(kept);
    r.files.put(kept.path, "XTC-new");
    r.library.save(r.files);
    const std::string orphan = downloadedEntry("Protocol", 50, 1).path;  // the old version, no longer indexed
    r.files.put(orphan, "XTC-old");
    r.files.bookTitles[orphan] = "Protocol";
    return std::make_pair(kept.path, orphan);
  };
  {
    Rig r;
    const auto paths = setup(r);
    method::Library again;
    const auto report = again.load(r.files);
    checkEq(report.adopted, size_t{0}, "the same title is on the shelf: the old file is not adopted");
    check(again.find(paths.second) == nullptr && r.files.has(paths.second), "it stays an orphan on the card");
    check(again.find(paths.first) != nullptr, "the new version is the shelf's");
  }
  {
    Rig r;  // the title leaves the shelf: now there is a free slot and no clash
    const auto paths = setup(r);
    method::Library shelf;
    shelf.load(r.files);
    shelf.remove(paths.first);
    shelf.save(r.files);
    r.files.remove(paths.first.c_str());
    method::Library again;
    const auto report = again.load(r.files);
    checkEq(report.adopted, size_t{1}, "with the title gone and room on the shelf it is adopted");
    check(again.find(paths.second) != nullptr, "and listed");
  }
}

void testWakeRetryLimit() {
  {
    WakeRig r(wake::BUDGET_MS, "", 12);
    r.publish("Stubborn", 4000, "b12");
    r.publish("Behind it", 3000, "b13");
    r.run();
    expectClean(r, "blocked book");
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "the blocked book is not requested");
    checkEq(r.check->filedCount(), 0u, "and the book behind it waits too");
    check(r.verdict(0) == wake::Verdict::Blocked, "the reason is the retry limit");
    checkEq(r.library.mark(), 0u, "the mark does not move");
  }
  {
    // A failed download names its book, so the wake can count it.
    WakeRig r;
    r.publish("Fails", 5000);
    r.http.routes[BOOK_ROUTE].stallAt = 2000;
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::DownloadFailed),
            "the stalled download fails");
    checkEq(r.check->failedBookId(), 12u, "and names the book");
    expectClean(r, "stalled on a wake");
    // A fresh run resets it.
    r.joinedByTheNoteCheck();
    r.http.routes[BOOK_ROUTE].stallAt = SIZE_MAX;
    r.run();
    checkEq(r.check->failedBookId(), 0u, "the next run starts clean");
    checkEq(r.check->filedCount(), 1u, "and files it");
  }
  {
    // A book that fails verification is not a failed wake: the check skips it, the mark moves past it.
    WakeRig r;
    r.publish("Corrupt", 4000);
    r.http.routes[BOOK_ROUTE].body[100] ^= 1;  // the bytes no longer match the announced digest
    r.run();
    checkEq(r.check->failedBookId(), 0u, "a book that fails verification is not counted as a failed download");
    checkEq(r.library.mark(), 12u, "the mark moves past it");
  }
}

void testWakeRunsStartClean() {
  // The same check object started again: the decisions and the failed book are the new run's alone.
  WakeRig r;
  r.publish("Fails", 5000);
  r.http.routes[BOOK_ROUTE].stallAt = 2000;
  r.run();
  checkEq(r.check->failedBookId(), 12u, "the first run names its failed book");
  checkEq(r.decided(), size_t{1}, "and records its decision");
  r.clock.ms += 100000;  // a long sleep later: the first run's start is far outside this run's budget
  r.joinedByTheNoteCheck();
  r.http.routes[BOOK_ROUTE].stallAt = SIZE_MAX;
  r.startedAt = r.clock.ms;
  r.check->start();
  checkEq(r.check->failedBookId(), 0u, "start() forgets the failed book");
  checkEq(r.decided(), size_t{0}, "and the decisions");
  for (int i = 0; i < 100000 && r.check->running(); ++i) {
    r.check->step();
    r.clock.ms += 10;
  }
  checkEq(r.check->filedCount(), 1u, "the second run files the book");
  checkEq(r.decided(), size_t{1}, "with one decision of its own");
  // The budget counts from the second start, not the first.
  check(r.elapsed() < 5000, "and the budget was measured from it");
}

void testWakeCleansUpOnEveryExit() {
  const auto base = [] {
    auto r = std::make_unique<WakeRig>();
    r->publish("Protocol", 8000);
    return r;
  };
  {
    auto r = base();
    r->http.routes[META_ROUTE].open = method::HttpOpen::Unauthorized;
    r->run();
    checkEq(static_cast<int>(r->check->result()), static_cast<int>(Result::AuthRefused), "list refused");
    expectClean(*r, "wake: list refused");
  }
  {
    auto r = base();
    r->http.routes[META_ROUTE].open = method::HttpOpen::Failed;
    r->run();
    checkEq(static_cast<int>(r->check->result()), static_cast<int>(Result::ServerError), "list failed");
    expectClean(*r, "wake: list failed");
  }
  {
    auto r = base();
    r->http.routes[BOOK_ROUTE].open = method::HttpOpen::Unauthorized;
    r->run();
    expectClean(*r, "wake: book refused");
  }
  {
    auto r = base();
    r->http.routes[BOOK_ROUTE].failAtEnd = true;
    r->run();
    checkEq(static_cast<int>(r->check->result()), static_cast<int>(Result::DownloadFailed), "failed at the end");
    expectClean(*r, "wake: failed at the end");
    check(r->library.entries().empty(), "wake: nothing filed");
  }
  {
    auto r = base();
    r->http.heapReady = false;
    r->run();
    checkEq(static_cast<int>(r->check->result()), static_cast<int>(Result::Memory), "no heap");
    expectClean(*r, "wake: no heap");
  }
  {
    auto r = base();
    r->files.free = 1000;
    r->run();
    checkEq(static_cast<int>(r->check->result()), static_cast<int>(Result::LowSpace), "no space");
    expectClean(*r, "wake: no space");
  }
  {
    auto r = base();
    r->files.failSinkWrite = true;
    r->run();
    expectClean(*r, "wake: card write failed");
  }
  {
    auto r = base();
    r->runUntil(Phase::Download, 5);
    r->check->cancel();
    expectClean(*r, "wake: cancelled in a download");
  }
  {
    // Nothing new: the radio still goes off.
    WakeRig r;
    r.republish();
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::UpToDate), "an empty mailbox");
    expectClean(r, "wake: nothing new");
    check(r.check->decisions().empty(), "wake: no book, no decision");
  }
}

// Stage B and C joined: the timers the list parser validated reach the library entry the check files, and a set that
// fails validation is dropped without losing the book.
void testTimersReachTheLibrary() {
  {
    Rig r;
    const Published p = r.publish("Example column clean-up");
    std::string& body = r.http.routes[META_ROUTE].body;
    const std::string tail = "\"book_url\":\"" + Rig::route(p.id) + "\"}";
    const size_t at = body.find(tail);
    check(at != std::string::npos, "timers test: the list entry is where the test expects it");
    body.replace(at, tail.size(),
                 "\"book_url\":\"" + Rig::route(p.id) +
                     "\",\"timers\":[{\"page\":3,\"seconds\":300,\"label\":\"Incubate\",\"box\":[32,610,416,96]}]}");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod), "timers test: the book is filed");
    expectClean(r, "timers reach the library");
    checkEq(r.library.entries().size(), size_t{1}, "timers test: one book on the shelf");
    const auto& timers = r.library.entries()[0].timers;
    checkEq(timers.size(), size_t{1}, "the filed entry carries the book's timer");
    if (timers.size() == 1) {
      checkEq(static_cast<unsigned>(timers[0].page), 3u, "timer page");
      checkEq(static_cast<unsigned>(timers[0].seconds), 300u, "timer seconds");
      checkEq(timers[0].label, std::string("Incubate"), "timer label");
      check(timers[0].x == 32 && timers[0].y == 610 && timers[0].w == 416 && timers[0].h == 96, "timer box");
    }
    check(r.files.files[method::INDEX_PATH].find("Incubate") != std::string::npos,
          "the timer is in the index written to the card");
  }
  {
    Rig r;
    const Published p = r.publish("Example column clean-up");
    std::string& body = r.http.routes[META_ROUTE].body;
    const std::string tail = "\"book_url\":\"" + Rig::route(p.id) + "\"}";
    const size_t at = body.find(tail);
    // Page 99 of an 8-page book: the whole set is dropped, the book kept.
    body.replace(at, tail.size(),
                 "\"book_url\":\"" + Rig::route(p.id) +
                     "\",\"timers\":[{\"page\":99,\"seconds\":300,\"box\":[32,610,416,96]}]}");
    r.run();
    checkEq(static_cast<int>(r.check->result()), static_cast<int>(Result::NewMethod),
            "invalid timers: the book is still filed");
    checkEq(r.library.entries().size(), size_t{1}, "invalid timers: one book on the shelf");
    check(r.library.entries().empty() || r.library.entries()[0].timers.empty(), "invalid timers are not kept");
  }
}

// A book re-sent with the same bytes has a new id and, possibly, different timers: they are not part of the bytes. The
// held entry takes the listed set (an index write, no download), and nothing else about it changes.
void testResendBringsTimers() {
  const std::string oneTimer = "[{\"page\":3,\"seconds\":300,\"label\":\"Incubate\",\"box\":[32,610,416,96]}]";
  const std::string otherTimer = "[{\"page\":4,\"seconds\":900,\"box\":[32,540,416,156]}]";
  const auto listWith = [](Rig& r, const Published& p, const std::string& timersJson) {
    std::string& body = r.http.routes[META_ROUTE].body;
    const std::string tail = "\"book_url\":\"" + Rig::route(p.id) + "\"}";
    const size_t at = body.find(tail);
    check(at != std::string::npos, "resend test: the list entry is where the test expects it");
    body.replace(at, tail.size(), "\"book_url\":\"" + Rig::route(p.id) + "\",\"timers\":" + timersJson + "}");
  };
  const auto hold = [](Rig& r, const Published& p, const std::vector<method::Timer>& timers, const std::string& sha) {
    method::Entry e;
    e.title = p.title;
    e.path = p.path;
    e.sha256 = sha;
    e.bookId = sha.empty() ? "" : "b9";
    e.pageCount = 8;
    e.opened = true;
    e.receivedAt = 555;
    e.timers = timers;
    r.library.add(e);
    r.library.advanceMark(9);
    r.files.put(p.path, p.bytes);
  };
  {  // The first timers arrive with a re-send of a book held without any.
    Rig r;
    const Published p = r.publish("Protocol", 4000, "b13");
    hold(r, p, {}, p.sha);
    listWith(r, p, oneTimer);
    r.run();
    checkEq(r.http.opened, (std::vector<std::string>{META_ROUTE}), "resend: no download");
    const auto* kept = r.library.find(p.path);
    check(kept && kept->timers.size() == 1 && kept->timers[0].page == 3 && kept->timers[0].seconds == 300,
          "resend: the held entry takes the timers");
    check(kept && kept->opened && kept->receivedAt == 555 && kept->bookId == "b9",
          "resend: and nothing else about it changes");
    checkEq(r.library.mark(), 13u, "resend: the mark moves");
    check(r.files.files[method::INDEX_PATH].find("Incubate") != std::string::npos &&
              r.files.files[method::INDEX_PATH] == r.library.serialise(),
          "resend: the index on the card has them");
    expectClean(r, "resend brings timers");
  }
  {  // Different timers replace the stored ones.
    Rig r;
    const Published p = r.publish("Protocol", 4000, "b13");
    method::Timer old;
    old.page = 3;
    old.seconds = 300;
    old.x = 32;
    old.y = 610;
    old.w = 416;
    old.h = 96;
    hold(r, p, {old}, p.sha);
    listWith(r, p, otherTimer);
    r.run();
    const auto* kept = r.library.find(p.path);
    check(kept && kept->timers.size() == 1 && kept->timers[0].page == 4 && kept->timers[0].seconds == 900,
          "resend: a different set replaces the stored one");
  }
  {  // The same timers: nothing to write but the mark.
    Rig r;
    const Published p = r.publish("Protocol", 4000, "b13");
    method::Timer same;
    same.page = 3;
    same.seconds = 300;
    same.label = "Incubate";
    same.x = 32;
    same.y = 610;
    same.w = 416;
    same.h = 96;
    hold(r, p, {same}, p.sha);
    listWith(r, p, oneTimer);
    r.run();
    const auto* kept = r.library.find(p.path);
    check(kept && kept->timers.size() == 1 && kept->timers[0] == same, "resend: the same set stays as it was");
  }
  {  // A re-send that lists none takes them away; one whose set does not validate leaves the stored ones alone.
    Rig r;
    const Published p = r.publish("Protocol", 4000, "b13");
    method::Timer old;
    old.page = 3;
    old.seconds = 300;
    old.x = 32;
    old.y = 610;
    old.w = 416;
    old.h = 96;
    hold(r, p, {old}, p.sha);
    r.run();  // the list carries no timers key
    check(r.library.find(p.path) && r.library.find(p.path)->timers.empty(),
          "resend: a book that now lists no timers has none");
    Rig bad;
    const Published q = bad.publish("Protocol", 4000, "b13");
    hold(bad, q, {old}, q.sha);
    listWith(bad, q, "[{\"page\":99,\"seconds\":300,\"box\":[32,610,416,96]}]");
    bad.run();
    check(bad.library.find(q.path) && bad.library.find(q.path)->timers.size() == 1,
          "resend: a set that does not validate leaves the stored timers alone");
    checkEq(bad.library.mark(), 13u, "resend: and the mark still moves");
  }
  {  // A book found on the card (no SHA-256) never takes timers.
    Rig r;
    const Published p = r.publish("Protocol", 4000, "b13");
    hold(r, p, {}, "");
    listWith(r, p, oneTimer);
    r.run();
    check(r.library.find(p.path) && r.library.find(p.path)->timers.empty(),
          "resend: a hand-copied book is never given timers");
  }
}

int main() {
  testHappyPath();
  testTimersReachTheLibrary();
  testResendBringsTimers();
  testProgressIsReportedInTens();
  testNothingNew();
  testNotConfiguredAndBattery();
  testWifiFallbacks();
  testClockPhase();
  testClockSyncNeverHoldsTheLoop();
  testWaitingForTheRenderTask();
  testSlowestPassIsReported();
  testDownloadLimitScalesWithTheBook();
  testNoContentLengthDoesNotConsumeTheBook();
  testUnreadableRowsAreReported();
  testNameLookupBeforeEveryRequest();
  testReadOnlyLibraryDoesNotCheck();
  testMailboxFailures();
  testSpace();
  testDownloadFailures();
  testVerifyFailures();
  testReplaceAndPruneThroughTheCheck();
  testSeveralNewBooks();
  testDownloadCap();
  testShaAlreadyOnTheCard();
  testMarkOnFailures();
  testGoneRereadsOnce();
  testMailboxReset();
  testReplacementAcrossKinds();
  testClockIsRefreshedDuringALongRun();
  testCancelBetweenBooks();
  testCancelEverywhere();
  testPhaseTimeouts();
  testRestart();
  testObserverOutcomes();
  testWakeFilesABookInTheSameSession();
  testWakeFilesTwoThenStops();
  testWakeBatteryFloor();
  testWakeSizeCap();
  testWakeBudget();
  testWakeLeavesTheBookOnThePanel();
  testAnUnindexedBookIsNotAlwaysAdopted();
  testWakeRetryLimit();
  testWakeRunsStartClean();
  testWakeCleansUpOnEveryExit();
  std::cout << "method check: " << fake::checkCount() << " checks passed\n";
  return 0;
}
