#include "render/backend.h"

#include <optional>
#include <string_view>

namespace blue::render {

std::string_view BackendName(Backend backend) {
  switch (backend) {
    case Backend::kVulkan:
      return "vulkan";
    case Backend::kD3d12:
      return "d3d12";
    case Backend::kMetal:
      return "metal";
  }
  return "?";
}

std::optional<Backend> ParseBackend(std::string_view name) {
  for (const Backend backend :
       {Backend::kVulkan, Backend::kD3d12, Backend::kMetal}) {
    if (name == BackendName(backend)) {
      return backend;
    }
  }
  return std::nullopt;
}

platform::WindowGraphics WindowGraphicsFor(Backend backend) {
  switch (backend) {
    case Backend::kVulkan:
      return platform::WindowGraphics::kVulkan;
    case Backend::kD3d12:
      return platform::WindowGraphics::kNone;
    case Backend::kMetal:
      return platform::WindowGraphics::kMetal;
  }
  return platform::WindowGraphics::kNone;
}

}  // namespace blue::render
