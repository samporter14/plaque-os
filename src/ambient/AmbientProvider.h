#pragma once

#include <cstdint>

class GfxRenderer;

namespace ambient {
enum class FetchResult : uint8_t { None, Updated, Unchanged, NotConfigured, Failed };

class AmbientProvider {
 public:
  virtual ~AmbientProvider() = default;
  virtual FetchResult fetchContent() = 0;
  virtual FetchResult getStatus() const = 0;
  virtual bool hasContent() const = 0;
  virtual bool render(GfxRenderer& renderer) = 0;
};
}  // namespace ambient
