#ifndef BLUE_RENDER_SWAPCHAIN_H_
#define BLUE_RENDER_SWAPCHAIN_H_

#include "render/metal_types.h"

#include "core/extent.h"
#include "platform/window.h"
#include "render/metal_context.h"

namespace blue::render {

// The window's CAMetalLayer, configured for the renderer: sRGB BGRA8
// drawables that are only ever rendered to, at most 3 in flight, display sync
// per vsync. The drawables' memory is made resident through the layer's own
// residency set, which is attached to the context's queue. The layer belongs
// to the window, which must outlive this object.
// Move-only. Not thread-safe.
class Swapchain {
 public:
  // Configures the layer of `window` for the device of `context`. A zero
  // `extent` is fatal.
  Swapchain(const MetalContext& context, const platform::Window& window,
            core::Extent2D extent, bool vsync);

  Swapchain(const Swapchain&) = delete;
  Swapchain& operator=(const Swapchain&) = delete;
  Swapchain(Swapchain&&) = default;
  Swapchain& operator=(Swapchain&&) = default;

  // Applies a new drawable size and display sync. Returns false, leaving the
  // layer untouched, for a zero `extent` (e.g. a minimized window). Drawables
  // acquired before keep their old size; the GPU may still be using them.
  [[nodiscard]] bool Recreate(core::Extent2D extent, bool vsync);

  // MTLPixelFormatBGRA8Unorm_sRGB: the hardware gamma-encodes our linear
  // shading output.
  [[nodiscard]] MTLPixelFormat format() const;
  [[nodiscard]] core::Extent2D extent() const { return extent_; }
  [[nodiscard]] bool vsync() const { return vsync_; }

  // Returns the next drawable, or nil when none became available within the
  // layer's timeout (one second). Present it with MetalContext::Commit().
  [[nodiscard]] id<CAMetalDrawable> NextDrawable() const;

 private:
  CAMetalLayer* layer_ = nil;
  core::Extent2D extent_{};
  bool vsync_ = true;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_SWAPCHAIN_H_
