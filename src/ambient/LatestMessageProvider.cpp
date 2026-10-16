#include "LatestMessageProvider.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "AmbientSettings.h"
#include "ReaderIdentity.h"
#include "network/HttpDownloader.h"
#include "network/MessageTls.h"

namespace ambient {
namespace {
constexpr char CACHE_DIR[] = "/.crosspoint/ambient";
constexpr char SLOT_PATHS[2][35] = {"/.crosspoint/ambient/frame0.bin", "/.crosspoint/ambient/frame1.bin"};

bool checkFile(const char* path, const char* expectedHash) {
  HalFile file;
  if (!Storage.openFileForRead("AMBIENT", path, file) || !validPayloadSize(file.fileSize())) return false;
  // One reusable transfer block keeps file hashing below the task stack budget.
  auto buffer = makeUniqueNoThrow<uint8_t[]>(1024);
  if (!buffer) {
    LOG_ERR("AMBIENT", "OOM: hash buffer");
    return false;
  }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
  for (size_t remaining = PAYLOAD_BYTES; ok && remaining;) {
    const size_t count = std::min(remaining, size_t{1024});
    ok = file.read(buffer.get(), count) == static_cast<int>(count) &&
         mbedtls_sha256_update(&sha, buffer.get(), count) == 0;
    remaining -= count;
  }
  uint8_t digest[32];
  ok = ok && mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  if (!ok) return false;
  char hex[65];
  for (size_t i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return strcmp(hex, expectedHash) == 0;
}
}  // namespace

LatestMessageProvider& LatestMessageProvider::instance() {
  static LatestMessageProvider provider;
  return provider;
}

bool LatestMessageProvider::cachedContentValid() const {
  return MESSAGE_STATE.lastMessageId[0] && checkFile(SLOT_PATHS[MESSAGE_STATE.slot], MESSAGE_STATE.sha256);
}

bool LatestMessageProvider::hasContent() const {
  std::lock_guard<std::mutex> lock(contentMutex);
  return cachedContentValid();
}

FetchResult LatestMessageProvider::getStatus() const {
  std::lock_guard<std::mutex> lock(contentMutex);
  return status;
}

FetchResult LatestMessageProvider::fetchContent() {
  std::lock_guard<std::mutex> lock(contentMutex);
  pending = {};
  const auto& config = AMBIENT_SETTINGS;
  if (!config.configured()) return status = FetchResult::NotConfigured;
  if (WiFi.status() != WL_CONNECTED) return status = FetchResult::Failed;
  // Manual checks and timer wakes share this path. Sync before constructing
  // credentials or opening TLS; failure preserves the current cached image.
  if (std::strncmp(config.baseUrl, "https://", 8) == 0 && !message_tls::prepareClock())
    return status = FetchResult::Failed;
  std::vector<HttpDownloader::Header> headers;
  headers.reserve(4);
  headers.emplace_back("Authorization", std::string("Bearer ") + config.deviceToken);
  appendIdentityHeaders(headers);  // X-Lamina-Reader, -Firmware and -Battery, on /latest and /image alike
  const std::string origin = config.baseUrl;
  const std::string url = origin + "/api/device/" + config.deviceId + "/latest";
  // Metadata is bounded to 2 KB; the bitmap itself is streamed straight to SD.
  auto body = makeUniqueNoThrow<char[]>(MAX_METADATA_BYTES + 1);
  if (!body) {
    LOG_ERR("AMBIENT", "OOM: metadata buffer");
    return status = FetchResult::Failed;
  }
  size_t used = 0;
  const auto fetched = HttpDownloader::fetchBounded(
      url,
      [&](const uint8_t* data, size_t len) {
        if (len > MAX_METADATA_BYTES - used) return false;
        memcpy(body.get() + used, data, len);
        used += len;
        return true;
      },
      headers, MAX_METADATA_BYTES);
  if (fetched != HttpDownloader::OK || !parseLatest({body.get(), used}, config.deviceId, pending)) {
    pending = {};
    return status = FetchResult::Failed;
  }
  body.reset();
  if (pending.empty) return status = FetchResult::None;
  if (unchanged(pending, MESSAGE_STATE.lastMessageId, MESSAGE_STATE.sha256, cachedContentValid())) {
    pending = {};
    return status = FetchResult::Unchanged;
  }
  if (!Storage.exists(CACHE_DIR) && !Storage.mkdir(CACHE_DIR)) {
    pending = {};
    return status = FetchResult::Failed;
  }
  pendingSlot = MESSAGE_STATE.slot ^ 1;
  HalFile file;
  if (!Storage.openFileForWrite("AMBIENT", SLOT_PATHS[pendingSlot], file)) {
    pending = {};
    return status = FetchResult::Failed;
  }
  const auto downloaded = HttpDownloader::fetchBounded(
      origin + pending.imagePath, [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; },
      headers, PAYLOAD_BYTES);
  // The same path must be reopened for validation, so close this local handle now.
  file.close();
  if (downloaded != HttpDownloader::OK || !checkFile(SLOT_PATHS[pendingSlot], pending.sha256)) {
    pending = {};
    return status = FetchResult::Failed;
  }
  return status = FetchResult::Updated;
}

bool LatestMessageProvider::render(GfxRenderer& renderer) {
  std::lock_guard<std::mutex> lock(contentMutex);
  const bool newContent = !pending.empty;
  const uint8_t slot = newContent ? pendingSlot : MESSAGE_STATE.slot;
  const char* hash = newContent ? pending.sha256 : MESSAGE_STATE.sha256;
  if (display.getDisplayWidth() != WIDTH || display.getDisplayHeight() != HEIGHT ||
      display.getBufferSize() != PAYLOAD_BYTES || !display.getFrameBuffer() || !checkFile(SLOT_PATHS[slot], hash))
    return false;
  HalFile file;
  if (!Storage.openFileForRead("AMBIENT", SLOT_PATHS[slot], file) ||
      file.read(display.getFrameBuffer(), PAYLOAD_BYTES) != static_cast<int>(PAYLOAD_BYTES))
    return false;
  // The wire image is in native landscape panel coordinates, independent of reader orientation.
  display.setInverted(false);
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  return !newContent || commitPendingLocked();
}

bool LatestMessageProvider::commitPendingLocked() {
  char oldId[65];
  char oldHash[65];
  memcpy(oldId, MESSAGE_STATE.lastMessageId, sizeof(oldId));
  memcpy(oldHash, MESSAGE_STATE.sha256, sizeof(oldHash));
  const uint8_t oldSlot = MESSAGE_STATE.slot;
  // The other SD slot remains usable until the state file commits the displayed version.
  MESSAGE_STATE.slot = pendingSlot;
  snprintf(MESSAGE_STATE.lastMessageId, sizeof(MESSAGE_STATE.lastMessageId), "%s", pending.messageId);
  snprintf(MESSAGE_STATE.sha256, sizeof(MESSAGE_STATE.sha256), "%s", pending.sha256);
  if (!MESSAGE_STATE.saveToFile()) {
    LOG_ERR("AMBIENT", "Failed to persist displayed message; next poll will retry");
    MESSAGE_STATE.slot = oldSlot;
    memcpy(MESSAGE_STATE.lastMessageId, oldId, sizeof(oldId));
    memcpy(MESSAGE_STATE.sha256, oldHash, sizeof(oldHash));
    return false;
  }
  pending = {};
  return true;
}

bool LatestMessageProvider::commitPending() {
  std::lock_guard<std::mutex> lock(contentMutex);
  // The frame was verified when it was downloaded; check it again before it becomes the current one.
  if (pending.empty || !checkFile(SLOT_PATHS[pendingSlot], pending.sha256)) return false;
  return commitPendingLocked();
}

}  // namespace ambient
