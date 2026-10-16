#include "MessageProtocol.h"

#include <ArduinoJson.h>

#include <algorithm>
#include <charconv>
#include <cstring>

namespace ambient {
namespace {
bool privateHost(std::string_view host) {
  if (host == "localhost" || (host.size() > 6 && host.substr(host.size() - 6) == ".local")) return true;
  unsigned octets[4] = {};
  for (int i = 0; i < 4; ++i) {
    const size_t end = host.find('.');
    const auto part = host.substr(0, end);
    const auto parsed = std::from_chars(part.data(), part.data() + part.size(), octets[i]);
    if (part.empty() || parsed.ec != std::errc{} || parsed.ptr != part.data() + part.size() || octets[i] > 255)
      return false;
    if (i == 3)
      return end == std::string_view::npos &&
             (octets[0] == 10 || octets[0] == 127 || (octets[0] == 192 && octets[1] == 168) ||
              (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31));
    if (end == std::string_view::npos) return false;
    host.remove_prefix(end + 1);
  }
  return false;
}

bool hexHash(std::string_view value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(),
                                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}  // namespace

bool validIdentifier(std::string_view value, size_t maxLength) {
  return !value.empty() && value.size() <= maxLength && std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
}

bool validOrigin(std::string_view value, bool allowLocalHttp) {
  const bool https = value.substr(0, 8) == "https://";
  const bool http = value.substr(0, 7) == "http://";
  if ((!https && !http) || (http && !allowLocalHttp) || value.size() > 191) return false;
  value.remove_prefix(https ? 8 : 7);
  if (value.empty() || value.find_first_of("/@?#\\ \t\r\n") != std::string_view::npos) return false;
  const size_t colon = value.find(':');
  const auto host = value.substr(0, colon);
  if (host.empty() || host.front() == '.' || host.back() == '.' || !std::all_of(host.begin(), host.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
      }))
    return false;
  if (colon != std::string_view::npos) {
    const auto port = value.substr(colon + 1);
    unsigned number = 0;
    const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
    if (port.empty() || parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || number == 0 ||
        number > 65535)
      return false;
  }
  return https || privateHost(host);
}

bool validPayloadSize(size_t size) { return size == PAYLOAD_BYTES; }

bool parseLatest(std::string_view json, std::string_view deviceId, MessageMetadata& result) {
  if (json.empty() || json.size() > MAX_METADATA_BYTES || !validIdentifier(deviceId, 32)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(3)) || !doc.is<JsonObject>() ||
      doc["message_id"].isUnbound())
    return false;
  if (doc["message_id"].isNull()) {
    result = {};
    return true;
  }
  if (!doc["message_id"].is<const char*>() || !doc["sha256"].is<const char*>() || !doc["image_url"].is<const char*>() ||
      !doc["format"].is<const char*>() || !doc["width"].is<unsigned>() || !doc["height"].is<unsigned>() ||
      !doc["byte_length"].is<unsigned>())
    return false;
  const auto idString = doc["message_id"].as<JsonString>();
  const auto hashString = doc["sha256"].as<JsonString>();
  const auto pathString = doc["image_url"].as<JsonString>();
  const auto formatString = doc["format"].as<JsonString>();
  const std::string_view id(idString.c_str(), idString.size());
  const std::string_view hash(hashString.c_str(), hashString.size());
  const std::string_view path(pathString.c_str(), pathString.size());
  const std::string_view format(formatString.c_str(), formatString.size());
  char expected[128];
  if (!validIdentifier(id, 64) || !hexHash(hash) || format != FORMAT || doc["width"].as<unsigned>() != WIDTH ||
      doc["height"].as<unsigned>() != HEIGHT || !validPayloadSize(doc["byte_length"].as<unsigned>()))
    return false;
  snprintf(expected, sizeof(expected), "/api/device/%.*s/image/%.*s", static_cast<int>(deviceId.size()),
           deviceId.data(), static_cast<int>(id.size()), id.data());
  if (path != expected) return false;
  memcpy(result.messageId, id.data(), id.size());
  result.messageId[id.size()] = '\0';
  memcpy(result.sha256, hash.data(), hash.size());
  result.sha256[hash.size()] = '\0';
  memcpy(result.imagePath, path.data(), path.size());
  result.imagePath[path.size()] = '\0';
  result.empty = false;
  return true;
}

bool unchanged(const MessageMetadata& metadata, std::string_view lastId, std::string_view lastHash, bool validCache) {
  return !metadata.empty && validCache && metadata.messageId == lastId && metadata.sha256 == lastHash;
}
}  // namespace ambient
