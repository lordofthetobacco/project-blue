#ifndef BLUE_RENDER_GPU_SCOPE_H_
#define BLUE_RENDER_GPU_SCOPE_H_

#include <cstddef>

namespace blue::render {

// A named span of GPU work measured by GpuTimer. To time a new pass, add an
// enumerator before kCount and its name to GpuScopeName(); the overlay lists
// every scope.
enum class GpuScope {
  kTotal,         // The whole frame.
  kShadow,        // The shadow cascades; absent in frames that skip them.
  kDepthPrepass,  // Scene depth of the camera view.
  kSsao,          // Ambient occlusion and its blur; absent when off.
  kScopeView,     // A rifle scope's picture (depth and shading); absent
                  // without one.
  kScene,         // The HDR scene pass: meshes, sky and the reflex sight.
  kMotionBlur,    // Weapon motion blur (and the sight after it); absent when
                  // skipped.
  kBloom,         // Downsample / upsample chain; absent when off.
  kTonemap,       // Bloom mix, exposure and tonemapping into the LDR image.
  kOverlay,       // Editor debug lines and selection outline on the LDR image.
  kPresent,       // LDR image to the swapchain, with FXAA when on.
  kUi,            // The ImGui overlay.
  kCount,         // Number of scopes; not a scope.
};

inline constexpr size_t kGpuScopeCount = static_cast<size_t>(GpuScope::kCount);

// Returns the display name of `scope`, e.g. "shadow".
[[nodiscard]] const char* GpuScopeName(GpuScope scope);

}  // namespace blue::render

#endif  // BLUE_RENDER_GPU_SCOPE_H_
