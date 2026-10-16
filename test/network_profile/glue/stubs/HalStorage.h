#pragma once
// The SD card, as an in-memory file map (FakeWorld.h Card): what NetworkProfileDevice.cpp uses of HalStorage.
#include <Arduino.h>
#include <fcntl.h>

#include <cstddef>
#include <cstdint>
#include <string>

enum class UsbDriveState : uint8_t { Unsupported, WaitingForHost, Connected, Ejected, Disconnected, IoError };
using oflag_t = int;

class HalFile {
  friend class HalStorage;
  std::string path_;
  size_t position_ = 0;
  bool open_ = false;
  bool writable_ = false;

 public:
  bool isOpen() const { return open_; }
  explicit operator bool() const { return open_; }
  size_t fileSize();
  int read(void* buffer, size_t count);
  size_t write(const uint8_t* buffer, size_t count);
  size_t write(const void* buffer, size_t count) { return write(static_cast<const uint8_t*>(buffer), count); }
  bool seekSet(size_t offset);
  void flush() {}
  bool close();
};

class HalStorage {
 public:
  bool ready() const;
  UsbDriveState usbDriveState() const;
  bool exists(const char* path);
  bool mkdir(const char* path, bool pFlag = true);
  bool remove(const char* path);
  bool replaceFile(const char* tmpPath, const char* path);
  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool readFileToString(const char* moduleName, const std::string& path, size_t cap, std::string& out);
  HalFile open(const char* path, oflag_t flags = 0);
  static HalStorage& getInstance();
};
#define Storage HalStorage::getInstance()
