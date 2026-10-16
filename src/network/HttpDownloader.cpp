#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <ResumableFetch.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_tls_errors.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <strings.h>

#include <cerrno>
#include <functional>
#include <string>

#include "MessageTls.h"
#include "WifiPowerSaveGuard.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const msg) { LOG_DBG("WOLFSSL", "%s", msg); }

// httpfail mirrors these so its mapping can be tested on the host.
static_assert(httpfail::HTTP_ERR_EAGAIN == ESP_ERR_HTTP_EAGAIN);
static_assert(httpfail::TLS_ERR_CANNOT_RESOLVE_HOSTNAME == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME);
static_assert(httpfail::TLS_ERR_CANNOT_CREATE_SOCKET == ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET);
static_assert(httpfail::TLS_ERR_UNSUPPORTED_PROTOCOL_FAMILY == ESP_ERR_ESP_TLS_UNSUPPORTED_PROTOCOL_FAMILY);
static_assert(httpfail::TLS_ERR_FAILED_CONNECT_TO_HOST == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST);
static_assert(httpfail::TLS_ERR_SOCKET_SETOPT_FAILED == ESP_ERR_ESP_TLS_SOCKET_SETOPT_FAILED);
static_assert(httpfail::TLS_ERR_CONNECTION_TIMEOUT == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT);
static_assert(httpfail::TLS_ERR_TCP_CLOSED_FIN == ESP_ERR_ESP_TLS_TCP_CLOSED_FIN);
static_assert(httpfail::TLS_ERR_SERVER_HANDSHAKE_TIMEOUT == ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT);
static_assert(httpfail::TLS_ERR_MBEDTLS_FIRST == ESP_ERR_MBEDTLS_CERT_PARTLY_OK);
static_assert(httpfail::TLS_ERR_MBEDTLS_LAST == ESP_ERR_MBEDTLS_SSL_READ_FAILED);
static_assert(httpfail::TLS_ERR_MBEDTLS_HANDSHAKE_FAILED == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED);
static_assert(httpfail::MBEDTLS_CERT_VERIFY_FAILED == -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED);
static_assert(httpfail::MBEDTLS_SSL_TIMEOUT == -MBEDTLS_ERR_SSL_TIMEOUT);
static_assert(httpfail::ERRNO_EAGAIN == EAGAIN);
static_assert(httpfail::ERRNO_ETIMEDOUT == ETIMEDOUT);

namespace {
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room.
constexpr int HTTP_TIMEOUT_MS = 60000;

// Existing reader and OTA fetches go through wolfSSL (it speaks TLS 1.3 and
// reads large bodies reliably). Plain-http URLs still use a
// WiFiClient here, so this is safe for non-TLS targets too. A body cut short
// mid-transfer resumes with a Range request (see ResumableFetch.h).
HttpDownloader::DownloadError runGetSecure(const std::string& url, const std::string& username,
                                           const std::string& password,
                                           const std::vector<HttpDownloader::Header>& headers,
                                           const freeink::FetchSink& sink, const bool* cancelFlag = nullptr,
                                           size_t* bytesOut = nullptr, const bool downgradeRedirectsToHttp = false) {
  WifiPowerSaveGuard psGuard;
  freeink::FetchOptions options;
  options.redirectToHttp = downgradeRedirectsToHttp;
  const freeink::FetchResult result = freeink::fetchResumable(
      url, options,
      [&](freeink::SecureHttpClient& http, const bool sameOrigin) {
        http.setTimeout(HTTP_TIMEOUT_MS);
        http.setInsecure();
        // setUserAgent replaces SecureHttpClient's built-in UA; addHeader would
        // append a second User-Agent header, which strict servers reject (aiohttp
        // answers 400 "Duplicate 'User-Agent' header found").
        http.setUserAgent("CrossPoint-ESP32-" CROSSPOINT_VERSION);
        // Credentials and caller headers stay with the starting origin; a
        // redirect elsewhere (or to plain http) gets neither.
        if (sameOrigin) {
          if (!username.empty() && !password.empty()) http.setBasicAuth(username, password);
          for (const auto& h : headers) http.addHeader(h.first, h.second);
        }
        LOG_DBG("HTTP", "wolfSSL GET: %s (heap %u, max block %u)", url.c_str(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());
      },
      sink, [cancelFlag] { return cancelFlag && *cancelFlag; });
  if (bytesOut) *bytesOut = result.bytes;

  if (result.aborted) return HttpDownloader::ABORTED;
  if (result.stopped) return HttpDownloader::FILE_ERROR;
  if (result.status == 401 || result.status == 403) {
    LOG_ERR("HTTP", "wolfSSL request unauthorized: status %d: %s", result.status, url.c_str());
    return HttpDownloader::UNAUTHORIZED;
  }
  if (result.status < 200 || result.status >= 300) {
    LOG_ERR("HTTP", "wolfSSL request failed: status %d: %s", result.status, url.c_str());
    return HttpDownloader::HTTP_ERROR;
  }
  if (!result.complete) {
    LOG_ERR("HTTP", "wolfSSL incomplete: got %zu of %zu bytes", result.bytes, result.total);
    return HttpDownloader::HTTP_ERROR;
  }
  return HttpDownloader::OK;
}

// How long one BoundedGet::read waits for data before reporting Idle, so the caller's loop keeps running. The wait
// can be spent more than once inside one call: esp_http_client_read keeps asking the transport until its buffer is
// full or a read times out, and every transport read waits up to this long. Input needs two consecutive polls to
// commit a press, so a pass must stay well under 100 ms: with the chunk sizes the shelf uses, 25 ms bounds a read at
// about 50 ms. Only BoundedGet uses it; fetchBounded (Messages) keeps its 5 s socket timeout.
constexpr int BOUNDED_READ_SLICE_MS = 25;

// The checks and client setup fetchBounded and BoundedGet share: they are what keeps a Bearer token on
// verified TLS (CA bundle plus certificate dates, no redirects).
bool boundedPreflight(const std::string& url, bool& https) {
  https = url.compare(0, 8, "https://") == 0;
  if (!https && url.compare(0, 7, "http://") != 0) return false;
  if (https && !message_tls::clockReady()) return false;
  if (https && (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
                ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC)) {
    LOG_ERR("HTTP", "Insufficient heap for verified TLS");
    return false;
  }
  return true;
}

esp_http_client_config_t boundedConfig(const std::string& url, const bool https) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 5000;
  config.disable_auto_redirect = true;
  config.buffer_size = 1024;
  if (https) config.crt_bundle_attach = message_tls::attachDateCheckedBundle;
  return config;
}

}  // namespace

HttpDownloader::DownloadError HttpDownloader::fetchBounded(const std::string& url, const DataCallback& onData,
                                                           const std::vector<Header>& headers, size_t maxBytes,
                                                           uint32_t totalTimeoutMs, size_t chunkBytes) {
  if (maxBytes == 0 || !onData || chunkBytes == 0) return HTTP_ERROR;
  bool https = false;
  if (!boundedPreflight(url, https)) return HTTP_ERROR;
  // Heap buffer avoids a 1 KB addition to the small activity task stack.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(chunkBytes);
  if (!buffer) {
    LOG_ERR("HTTP", "OOM: bounded transfer buffer");
    return FILE_ERROR;
  }
  WifiPowerSaveGuard guard;
  esp_http_client_config_t config = boundedConfig(url, https);
  auto client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "OOM: verified HTTP client");
    return HTTP_ERROR;
  }
  for (const auto& header : headers) esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
  esp_http_client_set_header(client, "Connection", "close");
  DownloadError result = HTTP_ERROR;
  const unsigned long started = millis();
  if (esp_http_client_open(client, 0) == ESP_OK) {
    const int64_t length = esp_http_client_fetch_headers(client);
    const int code = esp_http_client_get_status_code(client);
    if (code == 401 || code == 403) result = UNAUTHORIZED;
    if (code == 200 && length >= 0 && static_cast<uint64_t>(length) <= maxBytes) {
      size_t received = 0;
      bool ok = true;
      while (millis() - started < totalTimeoutMs) {
        const int count =
            esp_http_client_read(client, reinterpret_cast<char*>(buffer.get()), static_cast<int>(chunkBytes));
        if (count == 0) break;
        if (count < 0 || static_cast<size_t>(count) > maxBytes - received || !onData(buffer.get(), count)) {
          ok = false;
          break;
        }
        received += count;
        delay(1);
      }
      if (ok && received > 0 && esp_http_client_is_complete_data_received(client)) result = OK;
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (result != OK) LOG_ERR("HTTP", "Bounded request failed (%d)", result);
  return result;
}

struct BoundedGetEvents {
  // Runs inside esp_http_client_fetch_headers(), on the caller's task.
  static esp_err_t onEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data || !event->header_key || !event->header_value)
      return ESP_OK;
    if (strcasecmp(event->header_key, "Location") != 0) return ESP_OK;
    auto* self = static_cast<HttpDownloader::BoundedGet*>(event->user_data);
    httpfail::locationHost(event->header_value, self->last.locationHost, sizeof(self->last.locationHost));
    return ESP_OK;
  }
};

HttpDownloader::BoundedGet::BoundedGet() = default;

HttpDownloader::BoundedGet::~BoundedGet() { close(); }

void HttpDownloader::BoundedGet::close() {
  if (client) {
    auto handle = static_cast<esp_http_client_handle_t>(client);
    esp_http_client_close(handle);
    esp_http_client_cleanup(handle);
    client = nullptr;
  }
  powerSaveGuard.reset();
  length = -1;
  received = 0;
}

HttpDownloader::BoundedGet::Open HttpDownloader::BoundedGet::open(const std::string& url,
                                                                  const std::vector<Header>& headers,
                                                                  const size_t maxBytes) {
  close();
  last = {};
  last.failure = httpfail::Failure::Preflight;
  bool https = false;
  if (maxBytes == 0 || !boundedPreflight(url, https)) return Open::Failed;
  powerSaveGuard = makeUniqueNoThrow<WifiPowerSaveGuard>();
  if (!powerSaveGuard) {
    LOG_ERR("HTTP", "OOM: power-save guard");
    return Open::Failed;
  }
  esp_http_client_config_t config = boundedConfig(url, https);
  config.event_handler = &BoundedGetEvents::onEvent;
  config.user_data = this;
  auto handle = esp_http_client_init(&config);
  if (!handle) {
    LOG_ERR("HTTP", "OOM: verified HTTP client");
    close();
    return Open::Failed;
  }
  client = handle;
  for (const auto& header : headers) esp_http_client_set_header(handle, header.first.c_str(), header.second.c_str());
  esp_http_client_set_header(handle, "Connection", "close");
  const esp_err_t opened = esp_http_client_open(handle, 0);
  if (opened != ESP_OK) {
    httpfail::TransportError error;
    error.tlsError = esp_http_client_get_and_clear_last_tls_error(handle, &error.tlsCode, &error.certFlags);
    error.socketErrno = esp_http_client_get_errno(handle);
    last.failure = httpfail::classifyTransport(error);
    LOG_ERR("HTTP", "Bounded request could not connect: %s (open 0x%x, tls 0x%x, code -0x%x, flags 0x%x, errno %d)",
            httpfail::failureName(last.failure), static_cast<unsigned>(opened), static_cast<unsigned>(error.tlsError),
            static_cast<unsigned>(error.tlsCode < 0 ? -error.tlsCode : error.tlsCode),
            static_cast<unsigned>(error.certFlags), error.socketErrno);
    close();
    return Open::Failed;
  }
  const int64_t contentLength = esp_http_client_fetch_headers(handle);
  const int code = esp_http_client_get_status_code(handle);
  last.status = code;
  if (contentLength < 0 && code <= 0) {
    last.status = 0;
    last.failure = httpfail::classifyHeaderRead(contentLength);
    LOG_ERR("HTTP", "Bounded request: no reply (%s)", httpfail::failureName(last.failure));
    close();
    return Open::Failed;
  }
  last.failure = httpfail::Failure::Status;
  switch (httpfail::classifyStatus(code)) {
    case httpfail::StatusClass::Unauthorized:
      close();
      return Open::Unauthorized;
    case httpfail::StatusClass::Gone:
      close();
      return Open::Gone;
    case httpfail::StatusClass::Redirect:
      // Not followed, and nothing is sent to the place it names.
      last.failure = httpfail::Failure::Redirect;
      LOG_ERR("HTTP", "Bounded request answered %d (redirect to %s): not followed", code,
              last.locationHost[0] ? last.locationHost : "the same host");
      close();
      return Open::Redirect;
    case httpfail::StatusClass::Ok:
      if (contentLength >= 0 && static_cast<uint64_t>(contentLength) <= maxBytes) break;
      last.failure = httpfail::Failure::BadLength;
      [[fallthrough]];
    case httpfail::StatusClass::NotFound:
    case httpfail::StatusClass::ServerError:
    case httpfail::StatusClass::Other:
      LOG_ERR("HTTP", "Bounded request refused (status %d, length %lld)", code, static_cast<long long>(contentLength));
      close();
      return Open::Failed;
  }
  last.failure = httpfail::Failure::None;
  length = contentLength;
  limit = maxBytes;
  received = 0;
  // From here a read waits only a short slice, so the caller's loop keeps polling input. The 5 s socket
  // timeout above still bounded the connect, the handshake and the headers.
  esp_http_client_set_timeout_ms(handle, BOUNDED_READ_SLICE_MS);
  return Open::Ok;
}

HttpDownloader::BoundedGet::Read HttpDownloader::BoundedGet::read(uint8_t* buffer, const size_t capacity, size_t& got) {
  got = 0;
  if (!client || !buffer || capacity == 0) return Read::Failed;
  auto handle = static_cast<esp_http_client_handle_t>(client);
  const int count = esp_http_client_read(handle, reinterpret_cast<char*>(buffer), static_cast<int>(capacity));
  if (count > 0) {
    if (static_cast<size_t>(count) > limit - received) return Read::Failed;
    received += static_cast<size_t>(count);
    got = static_cast<size_t>(count);
    return Read::Data;
  }
  if (count == -ESP_ERR_HTTP_EAGAIN) return Read::Idle;
  if (count == 0 && received > 0 && esp_http_client_is_complete_data_received(handle)) return Read::Done;
  return Read::Failed;
}

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  freeink::FetchSink sink;
  sink.write = onData;
  return runGetSecure(url, username, password, {}, sink) == OK;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, const bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             const std::vector<Header>& headers,
                                                             bool downgradeRedirectsToHttp) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());

  // Stage in <dest>.part: a failed or cancelled download never replaces an
  // existing copy, and a partial file never sits under the real name.
  const std::string partPath = destPath + ".part";
  Storage.remove(partPath.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", partPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    return FILE_ERROR;
  }

  freeink::FetchSink sink;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };
  // Reopening for write truncates: the server restarted the body from byte 0.
  sink.rewind = [&file, &partPath] {
    file.close();
    return Storage.openFileForWrite("HTTP", partPath.c_str(), file);
  };
  sink.progress = progress;

  size_t downloaded = 0;
  const DownloadError result =
      runGetSecure(url, username, password, headers, sink, cancelFlag, &downloaded, downgradeRedirectsToHttp);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove. A failed rewind leaves no open handle.
  if (file.isOpen()) file.close();

  if (result != OK) {
    Storage.remove(partPath.c_str());
    return result;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(partPath.c_str());
    return HTTP_ERROR;
  }
  if (!Storage.replaceFile(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("HTTP", "Failed to move download into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return FILE_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", downloaded);
  return OK;
}
