// The only place the code base is compiled differently per enabled backend:
// BLUE_RENDERER_HAS_VULKAN / _D3D12 / _METAL (0 or 1) and
// BLUE_DEFAULT_RENDERER_NAME come from CMake (src/render/CMakeLists.txt).

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "core/result.h"
#include "platform/window.h"
#include "render/backend.h"
#include "render/renderer.h"
#include "render/renderer_backend.h"

#if !defined(BLUE_RENDERER_HAS_VULKAN) || !defined(BLUE_RENDERER_HAS_D3D12) || \
    !defined(BLUE_RENDERER_HAS_METAL) || !defined(BLUE_DEFAULT_RENDERER_NAME)
#error "CMake must define BLUE_RENDERER_HAS_* and BLUE_DEFAULT_RENDERER_NAME"
#endif

namespace blue::render {

// Each returns null when its API finds no suitable device; defined in the
// backend's directory.
#if BLUE_RENDERER_HAS_VULKAN
[[nodiscard]] std::unique_ptr<RendererBackend> CreateVulkanRenderer(
    const RendererOptions& options);
#endif
#if BLUE_RENDERER_HAS_D3D12
[[nodiscard]] std::unique_ptr<RendererBackend> CreateD3d12Renderer(
    const RendererOptions& options);
#endif
#if BLUE_RENDERER_HAS_METAL
[[nodiscard]] std::unique_ptr<RendererBackend> CreateMetalRenderer(
    const RendererOptions& options);
#endif

namespace {

// In the fallback order.
constexpr auto kCompiledBackends = std::to_array<Backend>({
#if BLUE_RENDERER_HAS_VULKAN
    Backend::kVulkan,
#endif
#if BLUE_RENDERER_HAS_D3D12
    Backend::kD3d12,
#endif
#if BLUE_RENDERER_HAS_METAL
    Backend::kMetal,
#endif
});

// Returns the renderer of `backend` for `options`, or null when it has no
// suitable device or is not compiled in.
std::unique_ptr<RendererBackend> TryCreate(Backend backend,
                                           const RendererOptions& options) {
  switch (backend) {
    case Backend::kVulkan:
#if BLUE_RENDERER_HAS_VULKAN
      return CreateVulkanRenderer(options);
#else
      return nullptr;
#endif
    case Backend::kD3d12:
#if BLUE_RENDERER_HAS_D3D12
      return CreateD3d12Renderer(options);
#else
      return nullptr;
#endif
    case Backend::kMetal:
#if BLUE_RENDERER_HAS_METAL
      return CreateMetalRenderer(options);
#else
      return nullptr;
#endif
  }
  return nullptr;
}

}  // namespace

std::span<const Backend> CompiledBackends() { return kCompiledBackends; }

bool IsCompiled(Backend backend) {
  for (const Backend compiled : CompiledBackends()) {
    if (compiled == backend) {
      return true;
    }
  }
  return false;
}

Backend DefaultBackend() {
  static const Backend kDefault = [] {
    const std::optional<Backend> backend =
        ParseBackend(BLUE_DEFAULT_RENDERER_NAME);
    if (!backend || !IsCompiled(*backend)) {
      core::log::Fatal("BLUE_DEFAULT_RENDERER '{}' is not a compiled backend",
                       BLUE_DEFAULT_RENDERER_NAME);
    }
    return *backend;
  }();
  return kDefault;
}

core::Result<Backend, std::string> ChooseBackend(
    std::optional<std::string_view> command_line,
    std::optional<std::string_view> environment) {
  using Chosen = core::Result<Backend, std::string>;
  std::string_view name;
  std::string_view source;
  if (command_line) {
    name = *command_line;
    source = "--renderer";
  } else if (environment && !environment->empty()) {
    name = *environment;
    source = "BLUE_RENDERER";
  } else {
    return Chosen::Ok(DefaultBackend());
  }
  const std::optional<Backend> backend = ParseBackend(name);
  if (backend && IsCompiled(*backend)) {
    return Chosen::Ok(*backend);
  }
  std::string compiled;
  for (const Backend each : CompiledBackends()) {
    if (!compiled.empty()) {
      compiled += ", ";
    }
    compiled += BackendName(each);
  }
  std::string message(source);
  message += backend ? ": renderer '" : ": unknown renderer '";
  message += name;
  message += backend ? "' is not compiled into this build" : "'";
  message += " (available: ";
  message += compiled;
  message += ")";
  return Chosen::Error(std::move(message));
}

std::unique_ptr<RendererBackend> CreateBackend(const RendererOptions& options) {
  if (!IsCompiled(options.backend)) {
    core::log::Fatal("Renderer backend '{}' is not compiled into this build",
                     BackendName(options.backend));
  }
  // The requested backend, then the default, then the fixed order.
  std::array<Backend, 2 + kCompiledBackends.size()> order{};
  size_t count = 0;
  const auto add = [&order, &count](Backend backend) {
    for (size_t i = 0; i < count; ++i) {
      if (order[i] == backend) {
        return;
      }
    }
    order[count++] = backend;
  };
  add(options.backend);
  add(DefaultBackend());
  for (const Backend backend : kCompiledBackends) {
    add(backend);
  }
  for (size_t i = 0; i < count; ++i) {
    const Backend backend = order[i];
    if (options.window != nullptr &&
        options.window->graphics() != WindowGraphicsFor(backend)) {
      // Switching would need a window created for that backend.
      core::log::Warn("Renderer backend '{}' skipped: the window was not "
                      "created for it",
                      BackendName(backend));
      continue;
    }
    std::unique_ptr<RendererBackend> renderer = TryCreate(backend, options);
    if (renderer == nullptr) {
      core::log::Warn("Renderer backend '{}' found no suitable device",
                      BackendName(backend));
      continue;
    }
    core::log::Info("Renderer backend: {}", BackendName(backend));
    return renderer;
  }
  core::log::Fatal("No renderer backend found a suitable device");
}

}  // namespace blue::render
