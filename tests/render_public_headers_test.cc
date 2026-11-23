// Includes every public renderer header from plain C++ (this file is not
// Objective-C++). Code outside src/render sees only these headers, so none of
// them may name a GPU API: the build fails here if one of them pulls Metal
// in (render/metal_types.h stops a plain C++ compile with an #error, and
// <Metal/Metal.h> itself does not compile as C++), Vulkan, D3D12, DXGI or
// <windows.h> (each defines the macro checked below).

#include <doctest/doctest.h>

#include "core/extent.h"
#include "platform/window.h"
#include "platform/window_graphics.h"
#include "render/backend.h"
#include "render/brdf_reference.h"
#include "render/debug_lines.h"
#include "render/draw_list.h"
#include "render/frame_config.h"
#include "render/frame_plan.h"
#include "render/frame_uniforms.h"
#include "render/gpu_scope.h"
#include "render/memory_stats.h"
#include "render/motion_blur_math.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/temporal_math.h"
#include "render/texture_types.h"
#include "ui/imgui_layer.h"
#include "world/draw_history.h"
#include "world/gpu_backend.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"
#include "world/world.h"
#include "world/world_physics.h"

#if defined(MTL_EXTERN)  // <Metal/MTLDefines.h>
#error "A public renderer header includes Metal"
#endif
#if defined(VK_VERSION_1_0) || defined(VULKAN_H_)  // <vulkan/vulkan.h>
#error "A public renderer header includes Vulkan"
#endif
#if defined(__d3d12_h__) || defined(__dxgi_h__)  // <d3d12.h>, <dxgi.h>
#error "A public renderer header includes D3D12 or DXGI"
#endif
#if defined(_WINDOWS_)  // <windows.h>
#error "A public renderer header includes <windows.h>"
#endif

namespace blue::core {

TEST_CASE("Extent2D compares width and height") {
  constexpr Extent2D kSize{.width = 1920, .height = 1080};
  CHECK(kSize == Extent2D{.width = 1920, .height = 1080});
  CHECK_FALSE(kSize == Extent2D{.width = 1080, .height = 1920});
  CHECK(Extent2D{} == Extent2D{.width = 0, .height = 0});
}

}  // namespace blue::core
