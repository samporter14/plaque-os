#include "MethodDevice.h"

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>
#include <lwip/tcpip.h>
#include <mbedtls/sha256.h>

#include <atomic>
#include <cstdint>
#include <ctime>

#include "RecentBooksStore.h"
#include "activities/RenderLock.h"
#include "ambient/AmbientSettings.h"
#include "ambient/MethodProtocol.h"
#include "ambient/ReaderIdentity.h"
#include "util/BookCacheUtils.h"

namespace method {
namespace {
constexpr char CACHE_ROOT[] = "/.crosspoint";

uint32_t readLe32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

unsigned heapFree() { return static_cast<unsigned>(ESP.getFreeHeap()); }
unsigned heapBlock() { return static_cast<unsigned>(ESP.getMaxAllocHeap()); }

// The name lookup. lwIP answers from its own task, possibly after the check that asked has gone (a lookup cannot be
// cancelled), so the callback touches only this word: the lookup's generation in the upper bits and its state (a
// ResolveStatus) in the lower two. The callback moves its own generation from Pending to its answer with a single
// compare-and-swap, so an answer to a lookup that was abandoned, or replaced by a newer one, changes nothing.
std::atomic<uint32_t> lookupWord{0};
constexpr uint32_t LOOKUP_STATE_MASK = 3;

uint32_t lookupWordFor(const uint32_t generation, const ResolveStatus state) {
  return (generation << 2) | static_cast<uint32_t>(state);
}

void lookupFound(const char*, const ip_addr_t* address, void* argument) {
  const uint32_t generation = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argument));
  uint32_t expected = lookupWordFor(generation, ResolveStatus::Pending);
  lookupWord.compare_exchange_strong(expected,
                                     lookupWordFor(generation, address ? ResolveStatus::Done : ResolveStatus::Failed));
}
}  // namespace

// ---- HTTP ----------------------------------------------------------------------------

// The first mailbox request of a boot makes the reader's identity and checks its copy on the card (a few NVS and SD
// operations). Asking for it here keeps that out of a transfer, where the shelf reads input between steps.
DeviceHttp::DeviceHttp(GfxRenderer& renderer) : renderer(renderer) { ambient::readerUuid(); }

// esp_http_client_open() resolves the host with getaddrinfo, which cannot be cancelled and has no timeout. So the name
// is looked up here first, through lwIP's own resolver, which answers from its task: the check polls for the answer
// and can give up. getaddrinfo then finds the entry in the same resolver table. dns_gethostbyname() uses the same
// address families as getaddrinfo(AF_UNSPEC), so the entry matches. lwIP is called under its core lock, as the
// sdkconfig (CONFIG_LWIP_TCPIP_CORE_LOCKING) requires of application code.
void DeviceHttp::beginResolve() {
  const uint32_t generation = (lookupWord.load() >> 2) + 1;
  lookupWord.store(lookupWordFor(generation, ResolveStatus::Pending));
  lookupStartedAt = millis();
  lookupLogged = false;
  const std::string host = urlHost(AMBIENT_SETTINGS.baseUrl);
  if (host.empty() || isIpv4Literal(host)) {
    lookupWord.store(lookupWordFor(generation, ResolveStatus::Done));  // nothing to look up
    return;
  }
  ip_addr_t address;
  LOCK_TCPIP_CORE();
  const err_t result = dns_gethostbyname(host.c_str(), &address, lookupFound,
                                         reinterpret_cast<void*>(static_cast<uintptr_t>(generation)));
  UNLOCK_TCPIP_CORE();
  // ERR_OK: already known (cached). ERR_INPROGRESS: lwIP calls back. Anything else: no resolver to ask.
  if (result == ERR_OK) {
    lookupWord.store(lookupWordFor(generation, ResolveStatus::Done));
  } else if (result != ERR_INPROGRESS) {
    lookupWord.store(lookupWordFor(generation, ResolveStatus::Failed));
  }
}

ResolveStatus DeviceHttp::resolveStatus() {
  const auto state = static_cast<ResolveStatus>(lookupWord.load() & LOOKUP_STATE_MASK);
  if (state != ResolveStatus::Pending && !lookupLogged) {
    lookupLogged = true;
    LOG_INF("METHOD", "name lookup %s in %u ms", state == ResolveStatus::Done ? "done" : "failed",
            static_cast<unsigned>(millis() - lookupStartedAt));
  }
  return state;
}

// An answer that arrives later finds a newer generation and is ignored.
void DeviceHttp::abortResolve() {
  lookupWord.store(lookupWordFor((lookupWord.load() >> 2) + 1, ResolveStatus::Pending));
  lookupLogged = true;
}

HttpReady DeviceHttp::ready() {
  // Rebuildable SD-font caches can hold tens of KB the TLS session needs; release them (they repopulate on
  // demand) and refuse to start below the floor, as OPDS does. The render task draws with those caches, and
  // releasing frees buffers its glyph pointers still point into, so this takes the render lock. It only tries:
  // blocking would wait out a panel refresh with no input read, so while a render is running the check is told
  // to ask again on its next step. This is the only place the Method code touches the renderer from the loop task.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock()) return HttpReady::Wait;
  if (auto* fontCache = renderer.getFontCacheManager()) fontCache->releaseSdFontCaches();
  if (heapFree() >= HttpDownloader::MIN_TLS_FREE_HEAP && heapBlock() >= HttpDownloader::MIN_TLS_MAX_ALLOC)
    return HttpReady::Yes;
  LOG_ERR("METHOD", "Low heap for TLS (%u free, %u max block)", heapFree(), heapBlock());
  return HttpReady::NoMemory;
}

HttpOpen DeviceHttp::open(const char* path, const size_t maxBytes) {
  const auto& config = AMBIENT_SETTINGS;
  // The Bearer token goes only to the configured origin, and only on this device's own book routes.
  refused = !config.configured() || !routeAllowed(config.deviceId, path);
  if (refused) return HttpOpen::Failed;
  std::vector<HttpDownloader::Header> headers;
  headers.reserve(5);
  headers.emplace_back("Authorization", std::string("Bearer ") + config.deviceToken);
  ambient::appendIdentityHeaders(headers);  // X-Lamina-Reader, -Firmware and -Battery, on /books and /book alike
  // Only the list carries timers, and the Worker sends them only to a reader that says it reads them (SPEC-DEVICES
  // section 3). routeAllowed() above already pinned the path to this device's /books or /book/b<n>.
  const std::string_view route(path);
  const std::string_view list("/books");
  if (route.size() >= list.size() && route.substr(route.size() - list.size()) == list)
    headers.emplace_back("X-Lamina-Features", method::LIST_FEATURES);
  const uint32_t started = millis();
  const auto opened = get.open(std::string(config.baseUrl) + path, headers, maxBytes);
  // After the name lookup above, open() should not wait for DNS again: a long open on the second and later requests
  // of a check (the first one pays the lookup) would mean the resolver's cache was missed.
  LOG_INF("METHOD", "open took %u ms", static_cast<unsigned>(millis() - started));
  switch (opened) {
    case HttpDownloader::BoundedGet::Open::Ok:
      return HttpOpen::Ok;
    case HttpDownloader::BoundedGet::Open::Unauthorized:
      return HttpOpen::Unauthorized;
    case HttpDownloader::BoundedGet::Open::Gone:
      return HttpOpen::Gone;
    case HttpDownloader::BoundedGet::Open::Redirect:  // never followed; to the shelf it is a failed request
    case HttpDownloader::BoundedGet::Open::Failed:
      break;
  }
  return HttpOpen::Failed;
}

httpfail::Outcome DeviceHttp::outcome() const {
  if (!refused) return get.outcome();
  httpfail::Outcome outcome;
  outcome.failure = httpfail::Failure::Preflight;
  return outcome;
}

HttpRead DeviceHttp::read(uint8_t* buffer, const size_t capacity, size_t& got) {
  switch (get.read(buffer, capacity, got)) {
    case HttpDownloader::BoundedGet::Read::Data:
      return HttpRead::Data;
    case HttpDownloader::BoundedGet::Read::Idle:
      return HttpRead::Idle;
    case HttpDownloader::BoundedGet::Read::Done:
      return HttpRead::Done;
    case HttpDownloader::BoundedGet::Read::Failed:
      break;
  }
  return HttpRead::Failed;
}

// ---- files ---------------------------------------------------------------------------

DeviceFiles::DeviceFiles() = default;
DeviceFiles::~DeviceFiles() = default;

bool DeviceFiles::exists(const char* path) { return Storage.exists(path); }

bool DeviceFiles::readAll(const char* path, const size_t maxBytes, std::string& out) {
  return Storage.readFileToString("METHOD", std::string(path), maxBytes, out);
}

int64_t DeviceFiles::fileSize(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("METHOD", path, file)) return -1;
  const int64_t size = static_cast<int64_t>(file.fileSize());
  file.close();
  return size;
}

bool DeviceFiles::writeFile(const char* path, const std::string_view data) {
  HalFile file;
  if (!Storage.openFileForWrite("METHOD", path, file)) return false;
  const size_t written = file.write(data.data(), data.size());
  file.flush();
  return file.close() && written == data.size();
}

bool DeviceFiles::replaceFile(const char* tmpPath, const char* destPath) {
  return Storage.replaceFile(tmpPath, destPath);
}

bool DeviceFiles::remove(const char* path) { return Storage.remove(path); }

bool DeviceFiles::ensureDirectory(const char* path) { return Storage.exists(path) || Storage.mkdir(path); }

std::vector<std::string> DeviceFiles::listBooks(const char* directory) {
  std::vector<std::string> names;
  if (!Storage.exists(directory)) return names;
  for (const auto& entry : Storage.listFiles(directory)) {
    const std::string name = entry.c_str();
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".xtc") == 0) names.push_back(name);
  }
  return names;
}

bool DeviceFiles::inspectBook(const char* path, BookInfo& info) {
  info = {};
  HalFile file;
  if (!Storage.openFileForRead("METHOD", path, file)) return false;
  info.bytes = file.fileSize();
  uint8_t magic[4] = {};
  // "XTC\0" (1-bit) or "XTCH" (2-bit): the two magics lib/Xtc accepts.
  info.magicOk = file.read(magic, sizeof(magic)) == static_cast<int>(sizeof(magic)) && magic[0] == 'X' &&
                 magic[1] == 'T' && magic[2] == 'C' && (magic[3] == 0 || magic[3] == 'H');
  file.close();
  if (!info.magicOk) return true;
  auto book = makeUniqueNoThrow<Xtc>(std::string(path), std::string(CACHE_ROOT));
  if (!book) {
    LOG_ERR("METHOD", "OOM: XTC check");
    return true;  // not parsed
  }
  if (book->load() && book->getPageCount() > 0) {
    info.parsed = true;
    info.pageCount = book->getPageCount();
    info.title = book->getTitle();
  }
  return true;
}

int64_t DeviceFiles::modifiedAt(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("METHOD", path, file)) return 0;
  const uint32_t packed = file.modificationTime();  // FAT date in the high half, time in the low half
  file.close();
  const uint16_t date = packed >> 16;
  const uint16_t clock = packed & 0xFFFF;
  if (date == 0) return 0;
  struct tm parts = {};
  parts.tm_year = (date >> 9) + 80;  // years since 1900; FAT counts from 1980
  parts.tm_mon = ((date >> 5) & 0x0F) - 1;
  parts.tm_mday = date & 0x1F;
  parts.tm_hour = clock >> 11;
  parts.tm_min = (clock >> 5) & 0x3F;
  parts.tm_sec = (clock & 0x1F) * 2;
  parts.tm_isdst = -1;
  if (parts.tm_year + 1900 < 2025) return 0;  // an unset card clock
  const time_t seconds = mktime(&parts);      // FAT stamps are local time
  return seconds < 0 ? 0 : static_cast<int64_t>(seconds);
}

int64_t DeviceFiles::savedPage(const char* bookPath) {
  // The XTC reader keeps the page index (little-endian, 4 bytes) in progress.bin next to its cache.
  const Xtc book{std::string(bookPath), std::string(CACHE_ROOT)};
  const std::string progress = book.getCachePath() + "/progress.bin";
  if (!Storage.exists(progress.c_str())) return -1;
  HalFile file;
  uint8_t data[4] = {};
  if (!Storage.openFileForRead("METHOD", progress, file) ||
      file.read(data, sizeof(data)) != static_cast<int>(sizeof(data)))
    return -1;
  return readLe32(data);
}

void DeviceFiles::removeBook(const char* bookPath) {
  clearBookCache(bookPath);
  if (!Storage.remove(bookPath)) LOG_ERR("METHOD", "Could not delete %s", bookPath);
  if (RECENT_BOOKS.removeByPath(bookPath)) RECENT_BOOKS.saveToFile();
  library::markLibraryIndexDirty();
}

void DeviceFiles::removeBookCache(const char* bookPath) { clearBookCache(bookPath); }

uint64_t DeviceFiles::freeBytes() { return Storage.freeBytes(); }

bool DeviceFiles::openForWrite(const char* path) {
  sink = makeUniqueNoThrow<HalFile>();
  if (!sink) return false;
  if (Storage.openFileForWrite("METHOD", path, *sink)) return true;
  sink.reset();
  return false;
}

bool DeviceFiles::write(const uint8_t* data, const size_t size) { return sink && sink->write(data, size) == size; }

bool DeviceFiles::closeWrite() {
  if (!sink) return true;
  sink->flush();
  const bool closed = sink->close();
  sink.reset();
  return closed;
}

// ---- hash ----------------------------------------------------------------------------

struct DeviceHash::Context {
  mbedtls_sha256_context sha;
};

DeviceHash::DeviceHash() = default;
DeviceHash::~DeviceHash() { abandon(); }

bool DeviceHash::begin() {
  abandon();
  context = makeUniqueNoThrow<Context>();
  if (!context) return false;
  mbedtls_sha256_init(&context->sha);
  if (mbedtls_sha256_starts(&context->sha, 0) == 0) return true;
  abandon();
  return false;
}

bool DeviceHash::update(const uint8_t* data, const size_t size) {
  return context && mbedtls_sha256_update(&context->sha, data, size) == 0;
}

bool DeviceHash::finish(char out[65]) {
  if (!context) return false;
  uint8_t digest[32] = {};
  const bool ok = mbedtls_sha256_finish(&context->sha, digest) == 0;
  abandon();
  if (ok) toHex(digest, out);
  return ok;
}

void DeviceHash::abandon() {
  if (!context) return;
  mbedtls_sha256_free(&context->sha);
  context.reset();
}

// ---- battery and log -----------------------------------------------------------------

int DeviceBattery::percent() { return powerManager.getBatteryPercentage(); }

void DeviceLog::phaseEntered(const char* name) {
  LOG_INF("METHOD", "phase %s: start (free=%u max_block=%u)", name, heapFree(), heapBlock());
}

void DeviceLog::phaseLeft(const char* name, const uint32_t elapsedMs, const char* outcome,
                          const uint32_t slowestPassMs) {
  LOG_INF("METHOD", "phase %s: %s after %u ms, longest loop pass %u ms (free=%u max_block=%u)", name, outcome,
          static_cast<unsigned>(elapsedMs), static_cast<unsigned>(slowestPassMs), heapFree(), heapBlock());
}

}  // namespace method
