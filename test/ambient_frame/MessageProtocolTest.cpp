#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "MessageProtocol.h"

namespace {
int checks = 0;
void check(bool condition, const char* name) {
  if (!condition) {
    std::cerr << "FAIL: " << name << '\n';
    std::exit(1);
  }
  ++checks;
}

std::string metadata(const std::string& id = "m1", const std::string& path = "/api/device/my-reader/image/m1",
                     const std::string& size = "48000", const std::string& width = "800") {
  return "{\"message_id\":\"" + id + "\",\"sha256\":\"" + std::string(64, 'a') + "\",\"image_url\":\"" + path +
         "\",\"width\":" + width + ",\"height\":480,\"byte_length\":" + size + ",\"format\":\"mono1-msb-white\"}";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 || argc == 8) {
    // A desktop poller can invoke the exact firmware parser and unchanged decision.
    if (std::filesystem::file_size(argv[1]) > ambient::MAX_METADATA_BYTES) return 2;
    std::ifstream stream(argv[1]);
    std::string body((std::istreambuf_iterator<char>(stream)), {});
    ambient::MessageMetadata result;
    if (!ambient::parseLatest(body, argv[3], result)) return 2;
    if (result.empty) {
      std::cout << "empty\n";
      return 0;
    }
    if (!ambient::validPayloadSize(std::filesystem::file_size(argv[2]))) return 2;
    const bool same = argc == 8 && ambient::unchanged(result, argv[4], argv[5], std::string(argv[6]) == "1");
    std::cout << (same ? "unchanged\n" : "updated\n");
    return 0;
  }
  check(argc == 1, "valid invocation");
  ambient::MessageMetadata result;
  check(ambient::parseLatest(metadata(), "my-reader", result), "valid metadata");
  check(!result.empty && std::string(result.messageId) == "m1", "message copied");
  check(ambient::unchanged(result, "m1", std::string(64, 'a'), true), "unchanged skips download/refresh");
  check(!ambient::unchanged(result, "m0", std::string(64, 'a'), true), "new version downloads");
  check(!ambient::unchanged(result, "m1", std::string(64, 'a'), false), "missing cache repairs");
  check(!ambient::unchanged(result, "m1", std::string(64, 'b'), true), "hash change repairs");
  check(ambient::parseLatest("{\"message_id\":null}", "my-reader", result) && result.empty, "empty mailbox");
  check(!ambient::parseLatest("{}", "my-reader", result), "missing id rejected");
  check(!ambient::parseLatest("[]", "my-reader", result), "array rejected");
  check(!ambient::parseLatest("{", "my-reader", result), "malformed JSON rejected");
  check(!ambient::parseLatest(metadata("../m1"), "my-reader", result), "path id rejected");
  check(!ambient::parseLatest(metadata("m1\\u0000suffix"), "my-reader", result), "embedded null rejected");
  check(!ambient::parseLatest(metadata("m1", "https://evil.example/image"), "my-reader", result),
        "foreign image origin rejected");
  check(!ambient::parseLatest(metadata("m1", "/api/device/other/image/m1"), "my-reader", result),
        "wrong device image rejected");
  check(!ambient::parseLatest(metadata("m1", "/api/device/my-reader/image/m1?token=bad"), "my-reader", result),
        "image query rejected");
  for (const auto* size : {"47999", "48001", "-1", "4294967296", "\"48000\""})
    check(!ambient::parseLatest(metadata("m1", "/api/device/my-reader/image/m1", size), "my-reader", result),
          "bad payload size rejected");
  check(!ambient::parseLatest(metadata("m1", "/api/device/my-reader/image/m1", "48000", "480"), "my-reader", result),
        "wrong dimensions rejected");
  check(!ambient::parseLatest(std::string(2049, ' '), "my-reader", result), "oversized metadata rejected");
  check(ambient::validPayloadSize(48000) && !ambient::validPayloadSize(47999) && !ambient::validPayloadSize(48001),
        "exact bitmap length");
  for (const auto* url : {"https://notes.example", "https://notes.example:8443", "http://192.168.1.10:8787",
                          "http://10.0.0.2", "http://172.16.0.1", "http://desk.local:8787", "http://localhost:8787"})
    check(ambient::validOrigin(url, true), "valid origin");
  for (const auto* url : {"http://public.example", "http://172.32.0.1", "https://host/path", "https://u:p@host",
                          "https://host:0", "https://host:99999", "https://host:abc", "https://host\r\nx: y",
                          "file:///tmp/image", "https://host?x", "http://192.168.999.1"})
    check(!ambient::validOrigin(url, true), "invalid origin rejected");
  check(!ambient::validOrigin("http://192.168.1.1", false), "plaintext disabled by default");
  std::cout << checks << " protocol checks passed\n";
}
