#include "render/backend.h"

#include <algorithm>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

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

std::vector<Backend> FallbackOrder(Backend requested, Backend default_backend,
                                   std::span<const Backend> compiled) {
  std::vector<Backend> order;
  const auto add = [&order, compiled](Backend backend) {
    if (std::ranges::find(compiled, backend) != compiled.end() &&
        std::ranges::find(order, backend) == order.end()) {
      order.push_back(backend);
    }
  };
  add(requested);
  add(default_backend);
  for (const Backend backend : compiled) {
    add(backend);
  }
  return order;
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
