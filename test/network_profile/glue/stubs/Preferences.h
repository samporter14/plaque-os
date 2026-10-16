#pragma once
// The Arduino Preferences API over the fake NVS (FakeWorld.h).
#include <cstddef>
#include <cstdint>
#include <string>

class Preferences {
  std::string space_;
  bool open_ = false;

 public:
  bool begin(const char* name, bool readOnly = false);
  void end() { open_ = false; }
  bool isKey(const char* key);
  size_t getBytesLength(const char* key);
  size_t getBytes(const char* key, void* buffer, size_t length);
  size_t putBytes(const char* key, const void* value, size_t length);
  int64_t getLong64(const char* key, int64_t fallback);
  size_t putLong64(const char* key, int64_t value);
  bool remove(const char* key);
};
