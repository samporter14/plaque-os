#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "MethodFormat.h"

// The edges of the Method shelf: everything the pure library and the pure check
// need from the outside world, as small interfaces. The firmware implements them
// over Arduino, ESP-IDF and the SD card (src/activities/ambient/MethodPorts.cpp);
// the host tests implement them with fakes. Nothing here includes Arduino or
// ESP-IDF headers.
//
// Threading: every call comes from the main loop task. Calls that can block say so.
namespace method {

// What the card says about one .xtc file.
struct BookInfo {
  uint64_t bytes = 0;
  bool magicOk = false;    // "XTC\0" or "XTCH"
  bool parsed = false;     // the XTC parser accepted the file
  uint32_t pageCount = 0;  // from the parser
  std::string title;       // metadata title, or the file name when the book has none
};

class FilePort {
 public:
  virtual ~FilePort() = default;
  virtual bool exists(const char* path) = 0;
  // Reads a whole small file; false when it is missing, empty (the card library refuses a zero-byte file),
  // larger than maxBytes or unreadable. fileSize() tells those apart.
  virtual bool readAll(const char* path, size_t maxBytes, std::string& out) = 0;
  // The file's size in bytes, or -1 when it is missing or cannot be opened.
  virtual int64_t fileSize(const char* path) = 0;
  virtual bool writeFile(const char* path, std::string_view data) = 0;  // create or truncate
  virtual bool replaceFile(const char* tmpPath, const char* destPath) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool ensureDirectory(const char* path) = 0;
  // Plain file names (no folder part) of the .xtc files directly inside `directory`.
  virtual std::vector<std::string> listBooks(const char* directory) = 0;
  virtual bool inspectBook(const char* path, BookInfo& info) = 0;
  // Unix time of the last change, or 0 when unknown.
  virtual int64_t modifiedAt(const char* path) = 0;
  // Page index saved in the book's progress.bin, or -1 when the book was never opened.
  virtual int64_t savedPage(const char* bookPath) = 0;
  // Deletes a book file, its reading cache and its Recent Books entry.
  virtual void removeBook(const char* bookPath) = 0;
  // Deletes only the reading cache (a new file under a reused name starts at page 1).
  virtual void removeBookCache(const char* bookPath) = 0;
  // Free bytes on the card; 0 means unknown.
  virtual uint64_t freeBytes() = 0;

  // One download sink at a time.
  virtual bool openForWrite(const char* path) = 0;
  virtual bool write(const uint8_t* data, size_t size) = 0;  // false on a short write
  virtual bool closeWrite() = 0;
};

enum class SyncStatus : uint8_t { Pending, Synced, Failed };

class ClockPort {
 public:
  virtual ~ClockPort() = default;
  virtual uint32_t nowMs() = 0;   // monotonic
  virtual int64_t unixNow() = 0;  // 0 when the clock is not trustworthy
  // Fills a calendar time in the reader's zone; false when `epoch` is 0 or unusable.
  virtual bool localTime(int64_t epoch, LocalTime& out) = 0;
  // True when the clock was synced recently enough for certificate dates.
  virtual bool clockReady() = 0;
  // Starts a sync. Neither call waits: the check polls syncStatus() once per step, so input is read between polls,
  // and enforces the clock limit itself. abortSync() is safe at any time, any number of times; it only abandons a
  // sync still in flight, and leaves clockReady() as it was.
  virtual void beginSync() = 0;
  virtual SyncStatus syncStatus() = 0;
  virtual void abortSync() = 0;
  // After a sync failed or ran out of time: may the retained clock stand in for it (SPEC-UNIVERSITY-WIFI section 7)?
  // True grants clockReady() on the clock the reader has kept since an earlier SNTP sync, within its error bound; false
  // leaves it as it was. The default is no fallback, for a port that has no retained clock to offer.
  virtual bool useRetained() { return false; }
};

enum class WifiStatus : uint8_t { Pending, Connected, Failed };
enum class ScanStatus : uint8_t { Pending, Done, Failed };

class WifiPort {
 public:
  virtual ~WifiPort() = default;
  virtual bool connected() = 0;
  virtual size_t savedCount() = 0;
  // Index of the saved network joined last, or -1.
  virtual int lastUsed() = 0;
  virtual void beginConnect(size_t saved) = 0;  // non-blocking
  virtual WifiStatus connectStatus() = 0;
  virtual void abortConnect() = 0;
  virtual void beginScan() = 0;  // non-blocking
  virtual ScanStatus scanStatus() = 0;
  // Saved networks in range, strongest first. Only valid after the scan completed.
  virtual std::vector<size_t> visibleSaved() = 0;
  virtual void noteJoined(size_t saved) = 0;  // remember it as the one to try first next time
  virtual void off() = 0;                     // radio off; safe to call at any time, any number of times
};

enum class HttpOpen : uint8_t { Ok, Unauthorized, Gone, Failed, NoMemory };  // Gone: 410, the book was replaced
enum class HttpReady : uint8_t { Yes, Wait, NoMemory };
enum class ResolveStatus : uint8_t { Pending, Done, Failed };
enum class HttpRead : uint8_t { Data, Idle, Done, Failed };

class HttpPort {
 public:
  virtual ~HttpPort() = default;
  // Can a TLS session start now? NoMemory: not enough heap, even after releasing what can be released. Wait: what
  // would be released is in use on another task right now (the render task draws with the font caches); the check
  // asks again on the next step instead of waiting, so input is still read, and its phase limit bounds the wait.
  virtual HttpReady ready() = 0;
  // The name lookup the next open() needs. esp_http_client_open resolves the host with getaddrinfo, which has no
  // timeout and cannot be cancelled, so on a network with no working DNS Back, Home and sleep would hang in it. The
  // check resolves the name first, one poll per step, with a time limit of its own: open() then finds the answer in
  // the resolver's cache. Done at once for an IP address or an answer that is already cached. abortResolve() is safe
  // at any time, any number of times; an answer that arrives afterwards is ignored.
  virtual void beginResolve() = 0;
  virtual ResolveStatus resolveStatus() = 0;
  virtual void abortResolve() = 0;
  // GET <configured origin><path> with the device token. Blocks only for the connection, the
  // TLS handshake and the response headers, each bounded by the 5 s socket timeout.
  virtual HttpOpen open(const char* path, size_t maxBytes) = 0;
  virtual int64_t contentLength() = 0;
  // Waits a short slice for data (at most a few hundred ms), never the whole socket timeout.
  virtual HttpRead read(uint8_t* buffer, size_t capacity, size_t& got) = 0;
  virtual void close() = 0;  // idempotent; frees the connection
};

class HashPort {
 public:
  virtual ~HashPort() = default;
  virtual bool begin() = 0;
  virtual bool update(const uint8_t* data, size_t size) = 0;
  // Lowercase hex digest in out[0..63] plus a terminating NUL; releases the context.
  virtual bool finish(char out[65]) = 0;
  virtual void abandon() = 0;  // releases the context without a result
};

class BatteryPort {
 public:
  virtual ~BatteryPort() = default;
  virtual int percent() = 0;
};

// Optional: phase timings for the serial log. slowestPassMs is the longest time between two steps of the check
// during the phase: the longest the main loop went without reading input.
class Observer {
 public:
  virtual ~Observer() = default;
  virtual void phaseEntered(const char* name) = 0;
  virtual void phaseLeft(const char* name, uint32_t elapsedMs, const char* outcome, uint32_t slowestPassMs) = 0;
};

}  // namespace method
