#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ambient {
constexpr uint16_t WIDTH = 800;
constexpr uint16_t HEIGHT = 480;
constexpr size_t PAYLOAD_BYTES = WIDTH * HEIGHT / 8;
constexpr size_t MAX_METADATA_BYTES = 2048;
constexpr char FORMAT[] = "mono1-msb-white";

struct MessageMetadata {
  char messageId[65] = {};
  char sha256[65] = {};
  char imagePath[160] = {};
  bool empty = true;
};

bool validIdentifier(std::string_view value, size_t maxLength);
bool validOrigin(std::string_view value, bool allowLocalHttp);
bool validPayloadSize(size_t size);
bool parseLatest(std::string_view json, std::string_view deviceId, MessageMetadata& result);
bool unchanged(const MessageMetadata& metadata, std::string_view lastId, std::string_view lastHash, bool validCache);
}  // namespace ambient
