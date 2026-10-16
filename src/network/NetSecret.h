#pragma once

#include <mbedtls/platform_util.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>

// Buffers that hold a network secret (a PSK, a private key, a bundle on its way in). Each is allocated once at its
// final size and never reallocated, so no stale copy is left behind in the heap, and is zeroed before it is freed.
namespace netprofile {

inline void wipe(void* data, const size_t size) {
  if (data && size) mbedtls_platform_zeroize(data, size);
}

class SecretBytes {
  uint8_t* bytes_ = nullptr;
  size_t size_ = 0;
  size_t capacity_ = 0;

 public:
  SecretBytes() = default;
  ~SecretBytes() { reset(); }
  SecretBytes(const SecretBytes&) = delete;
  SecretBytes& operator=(const SecretBytes&) = delete;
  SecretBytes(SecretBytes&& other) noexcept : bytes_(other.bytes_), size_(other.size_), capacity_(other.capacity_) {
    other.bytes_ = nullptr;
    other.size_ = other.capacity_ = 0;
  }
  SecretBytes& operator=(SecretBytes&& other) noexcept {
    if (this != &other) {
      reset();
      bytes_ = other.bytes_;
      size_ = other.size_;
      capacity_ = other.capacity_;
      other.bytes_ = nullptr;
      other.size_ = other.capacity_ = 0;
    }
    return *this;
  }

  // A zeroed buffer of `size` bytes (plus one spare zero byte, so text can be handed to C as a string). False on OOM.
  bool allocate(const size_t size) {
    reset();
    bytes_ = new (std::nothrow) uint8_t[size + 1]();
    if (!bytes_) return false;
    size_ = size;
    capacity_ = size + 1;
    return true;
  }

  bool assign(const void* data, const size_t size) {
    if (!allocate(size)) return false;
    if (size) std::memcpy(bytes_, data, size);
    return true;
  }

  // Shortens the logical size in place; the cut-off tail is zeroed.
  void shrink(const size_t size) {
    if (size >= size_) return;
    wipe(bytes_ + size, size_ - size);
    size_ = size;
  }

  void reset() {
    if (bytes_) {
      wipe(bytes_, capacity_);
      delete[] bytes_;
    }
    bytes_ = nullptr;
    size_ = capacity_ = 0;
  }

  uint8_t* data() { return bytes_; }
  const uint8_t* data() const { return bytes_; }
  // NUL-terminated: the spare byte past size() is always zero.
  const char* c_str() const { return bytes_ ? reinterpret_cast<const char*>(bytes_) : ""; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  std::string_view view() const { return {c_str(), size_}; }
};

}  // namespace netprofile
