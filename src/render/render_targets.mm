#include "render/render_targets.h"

#include <algorithm>
#include <cstdint>

#include "render/encoders.h"
#include "render/motion_blur_math.h"
#include "render/texture_types.h"

namespace blue::render {

namespace {

constexpr MTLTextureUsage kSampledTarget =
    MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;

Image MakeTarget(const MetalContext& context, core::Extent2D extent,
                 MTLPixelFormat format, const char* label,
                 uint32_t mip_levels = 1) {
  return Image(context, {
                            .extent = extent,
                            .format = format,
                            .usage = kSampledTarget,
                            .mip_levels = mip_levels,
                            .label = label,
                        });
}

}  // namespace

RenderTargets::RenderTargets(const MetalContext& context,
                             core::Extent2D extent)
    : context_(context) {
  Resize(extent);
}

core::Extent2D RenderTargets::bloom_mip_extent(uint32_t level) const {
  return {std::max(half_extent_.width >> level, 1u),
          std::max(half_extent_.height >> level, 1u)};
}

void RenderTargets::Resize(core::Extent2D extent) {
  extent_ = extent;
  half_extent_ = {std::max((extent.width + 1) / 2, 1u),
                  std::max((extent.height + 1) / 2, 1u)};

  hdr_ = MakeTarget(context_, extent, kHdrFormat, "HDR target");
  depth_ = MakeTarget(context_, extent, kDepthFormat, "Depth target");
  ldr_ = MakeTarget(context_, extent, kLdrFormat, "LDR target");
  ssao_raw_ =
      MakeTarget(context_, half_extent_, kSsaoRawFormat, "SSAO raw target");
  ssao_blur_ =
      MakeTarget(context_, half_extent_, kSsaoRawFormat, "SSAO blur target");
  ssao_ = MakeTarget(context_, half_extent_, kSsaoFormat, "SSAO target");
  selection_mask_ = MakeTarget(context_, extent, kSelectionMaskFormat,
                               "Selection mask target");

  motion_tile_size_ = MotionBlurMaxRadius(extent.height);
  const auto tile_size = static_cast<uint32_t>(motion_tile_size_);
  velocity_ = MakeTarget(context_, extent, kVelocityFormat, "Velocity target");
  motion_tiles_ = MakeTarget(context_,
                             {(extent.width + tile_size - 1) / tile_size,
                              (extent.height + tile_size - 1) / tile_size},
                             kMotionTileFormat, "Motion tiles target");
  motion_source_ =
      MakeTarget(context_, extent, kHdrFormat, "Motion blur source");

  bloom_mip_views_.clear();
  const uint32_t levels =
      std::min(kMaxBloomLevels, MipLevelCount(half_extent_));
  bloom_ = MakeTarget(context_, half_extent_, kBloomFormat, "Bloom target",
                      levels);
  for (uint32_t level = 0; level < levels; ++level) {
    bloom_mip_views_.push_back(
        bloom_.CreateView({.base_mip = level, .mip_count = 1}));
  }
  ClearSsaoToWhite();
}

void RenderTargets::ClearSsaoToWhite() {
  MTL4RenderPassDescriptor* pass = [[MTL4RenderPassDescriptor alloc] init];
  pass.colorAttachments[0].texture = ssao_.texture();
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 1.0, 1.0, 1.0);
  context_.SubmitImmediate([pass](const Recording& rec) {
    [BeginRenderPass(rec, pass, "Clear SSAO target") endEncoding];
  });
}

}  // namespace blue::render
