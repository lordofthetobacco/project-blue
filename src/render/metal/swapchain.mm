#include "render/metal/swapchain.h"

#include <CoreGraphics/CoreGraphics.h>

#include "core/log.h"

namespace blue::render::metal {

namespace {

constexpr MTLPixelFormat kFormat = MTLPixelFormatBGRA8Unorm_sRGB;
// Drawables the layer hands out before NextDrawable() blocks: one being
// displayed, one queued, one being rendered.
constexpr NSUInteger kMaxDrawables = 3;

}  // namespace

Swapchain::Swapchain(const MetalContext& context,
                     const platform::Window& window, core::Extent2D extent,
                     bool vsync)
    : layer_((__bridge CAMetalLayer*)window.metal_layer()) {
  if (layer_ == nil) {
    core::log::Fatal("The window has no CAMetalLayer");
  }
  layer_.device = context.device();
  layer_.pixelFormat = kFormat;
  // Drawables are only render targets, never sampled or copied, which lets
  // Metal pick the fastest memory layout for display.
  layer_.framebufferOnly = YES;
  layer_.maximumDrawableCount = kMaxDrawables;
  // Tells the compositor the drawables hold sRGB values, so they are
  // color-matched correctly on wide-gamut (P3) displays. The color space is a
  // Core Foundation object, which ARC does not manage.
  const CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  layer_.colorspace = srgb;
  CGColorSpaceRelease(srgb);

  // Metal 4 only renders to memory a residency set of the queue lists; the
  // layer keeps its drawables in this one (valid while the device is set).
  [context.queue() addResidencySet:layer_.residencySet];

  if (!Recreate(extent, vsync)) {
    core::log::Fatal("Cannot create a swapchain for a zero-sized surface");
  }
}

bool Swapchain::Recreate(core::Extent2D extent, bool vsync) {
  if (extent.width == 0 || extent.height == 0) {
    return false;
  }
  layer_.drawableSize = CGSizeMake(extent.width, extent.height);
  layer_.displaySyncEnabled = vsync;
  extent_ = extent;
  vsync_ = vsync;
  return true;
}

MTLPixelFormat Swapchain::format() const { return kFormat; }

id<CAMetalDrawable> Swapchain::NextDrawable() const {
  return [layer_ nextDrawable];
}

}  // namespace blue::render::metal
