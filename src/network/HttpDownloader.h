#pragma once
#include <HalStorage.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "HttpFailure.h"

struct WifiPowerSaveGuard;
struct BoundedGetEvents;

/**
 * HTTP client utility for fetching content and downloading files. Built on
 * esp_http_client: https is verified against the CA bundle, plain http is
 * used for local servers (transport is chosen from the URL scheme).
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    UNAUTHORIZED,  // 401/403: callers holding a refreshable credential can retry
  };

  // Pre-flight floor for starting a TLS transfer. Below this the session or
  // its ~17KB record buffer fails mid-stream (wolfSSL MEMORY_E) -- or an
  // interior allocation abort()s the device. Callers should check before
  // downloadToFile() and fail into their error UI instead.
  static constexpr uint32_t MIN_TLS_FREE_HEAP = 40000;
  static constexpr uint32_t MIN_TLS_MAX_ALLOC = 20000;

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  using Header = std::pair<std::string, std::string>;

  // Bounded, non-redirecting GET for private small resources. HTTPS verifies
  // both the certificate chain (IDF CA bundle) and hostname. Callers validate
  // any explicit local-development HTTP origin before calling this method.
  //
  // totalTimeoutMs bounds the whole transfer (default 20 s, sized for the small
  // Messages payloads); a multi-hundred-KB body passes a larger budget. A stalled
  // socket still times out after 5 s regardless. chunkBytes is the read size.
  static DownloadError fetchBounded(const std::string& url, const DataCallback& onData,
                                    const std::vector<Header>& headers, size_t maxBytes,
                                    uint32_t totalTimeoutMs = 20000, size_t chunkBytes = 1024);

  // The same request as fetchBounded (verified TLS, no redirects, a fresh clock sync for https, the
  // heap floor, Wi-Fi power save off for the whole transfer), but resumable: open() connects and reads the
  // headers, then read() hands back one chunk per call and returns within a short slice even when the
  // server is silent. The caller keeps servicing input between calls. open() may block for the connect,
  // the TLS handshake and the headers, each bounded by the 5 s socket timeout.
  //
  // Redirects are never followed (auto-redirect is off, and open() only reads the headers): a 3xx answers
  // Redirect, and outcome() names the Location host. outcome() also tells the other failures apart.
  class BoundedGet {
   public:
    enum class Open { Ok, Unauthorized, Gone, Redirect, Failed };  // Gone: 410
    enum class Read { Data, Idle, Done, Failed };

    BoundedGet();
    ~BoundedGet();
    BoundedGet(const BoundedGet&) = delete;
    BoundedGet& operator=(const BoundedGet&) = delete;

    // Rejects anything but a response with Content-Length of at most maxBytes.
    Open open(const std::string& url, const std::vector<Header>& headers, size_t maxBytes);
    int64_t contentLength() const { return length; }
    // Data: `got` bytes are in buffer. Idle: nothing arrived within the slice. Done: the whole body was read.
    Read read(uint8_t* buffer, size_t capacity, size_t& got);
    void close();  // idempotent; frees the connection and restores Wi-Fi power save
    // Why the last open() did not answer Ok (failure None when it did), its status and any Location host.
    const httpfail::Outcome& outcome() const { return last; }

   private:
    friend struct ::BoundedGetEvents;  // keeps the Location host from the reply's headers
    void* client = nullptr;            // esp_http_client_handle_t
    httpfail::Outcome last;
    std::unique_ptr<WifiPowerSaveGuard> powerSaveGuard;
    int64_t length = -1;
    size_t received = 0;
    size_t limit = 0;
  };

  /**
   * Download a file to the SD card with optional credentials. `headers` are
   * added to the request (e.g. a Bearer Authorization), alongside any Basic
   * auth derived from username/password.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, const bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      const std::vector<Header>& headers = {}, bool downgradeRedirectsToHttp = false);
};
