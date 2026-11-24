#ifndef BLUE_RENDER_BACKEND_H_
#define BLUE_RENDER_BACKEND_H_

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "platform/window_graphics.h"

namespace blue::render {

// The GPU APIs the renderer can run on. Which are compiled in depends on the
// platform and the CMake options BLUE_RENDERER_VULKAN / _D3D12 / _METAL.
enum class Backend : uint8_t {
  kVulkan,
  kD3d12,
  kMetal,
};

// Which adapter (GPU) a backend may pick.
enum class AdapterPreference : uint8_t {
  kHighPerformance,  // Discrete over integrated over anything else.
  kSoftware,         // CPU implementations only (WARP, lavapipe, llvmpipe).
};

// Returns the name of `backend` as the command line and BLUE_RENDERER take
// it: "vulkan", "d3d12" or "metal".
[[nodiscard]] std::string_view BackendName(Backend backend);
// Returns the backend named `name` (BackendName()), compiled in or not;
// nullopt for any other text.
[[nodiscard]] std::optional<Backend> ParseBackend(std::string_view name);

// Returns the backends compiled into this build, in the fallback order
// Vulkan, D3D12, Metal; never empty.
[[nodiscard]] std::span<const Backend> CompiledBackends();
// Returns the backend used when neither the command line nor BLUE_RENDERER
// names one (CMake option BLUE_DEFAULT_RENDERER); always compiled in.
[[nodiscard]] Backend DefaultBackend();
// Returns true when `backend` is compiled into this build.
[[nodiscard]] bool IsCompiled(Backend backend);

// Returns how the window must be created for `backend`.
[[nodiscard]] platform::WindowGraphics WindowGraphicsFor(Backend backend);

// Returns the backends to try, in order, when starting `requested` (design
// §1.4): `requested`, then `default_backend`, then `compiled` in its order
// (Vulkan, D3D12, Metal), each once and only if it is in `compiled`. An
// explicit --renderer= choice falls back too: the design makes only an
// unknown or not-compiled name fatal.
[[nodiscard]] std::vector<Backend> FallbackOrder(
    Backend requested, Backend default_backend,
    std::span<const Backend> compiled);

// Picks the backend to start with: the one named by `command_line` (the
// value of --renderer=) when set, else by `environment` (BLUE_RENDERER) when
// set and not empty, else DefaultBackend(). A name that is unknown or not
// compiled in is an error whose message lists CompiledBackends().
[[nodiscard]] core::Result<Backend, std::string> ChooseBackend(
    std::optional<std::string_view> command_line,
    std::optional<std::string_view> environment);

}  // namespace blue::render

#endif  // BLUE_RENDER_BACKEND_H_
