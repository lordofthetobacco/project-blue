// Includes every public renderer header from plain C++ (this file is not
// Objective-C++). Code outside src/render sees only these headers, so none of
// them may name the GPU API: the build fails here if one of them pulls Metal
// in (render/metal_types.h stops a plain C++ compile with an #error, and
// <Metal/Metal.h> itself does not compile as C++).

#include <doctest/doctest.h>

#include "core/extent.h"
#include "platform/window.h"
#include "render/brdf_reference.h"
#include "render/debug_lines.h"
#include "render/frame_config.h"
#include "render/frame_uniforms.h"
#include "render/gpu_scope.h"
#include "render/renderer.h"
#include "render/scene_view.h"
#include "render/texture_types.h"

#if defined(MTL_EXTERN)  // <Metal/MTLDefines.h>
#error "A public renderer header includes Metal"
#endif

namespace blue::core {

TEST_CASE("Extent2D compares width and height") {
  constexpr Extent2D kSize{.width = 1920, .height = 1080};
  CHECK(kSize == Extent2D{.width = 1920, .height = 1080});
  CHECK_FALSE(kSize == Extent2D{.width = 1080, .height = 1920});
  CHECK(Extent2D{} == Extent2D{.width = 0, .height = 0});
}

}  // namespace blue::core
