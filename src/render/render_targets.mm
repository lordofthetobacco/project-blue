#include "render/render_targets.h"

#include <algorithm>
#include <cstdint>

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
  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = ssao_.texture();
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 1.0, 1.0, 1.0);
  context_.SubmitImmediate([pass](id<MTLCommandBuffer> cmd) {
    id<MTLRenderCommandEncoder> encoder =
        [cmd renderCommandEncoderWithDescriptor:pass];
    encoder.label = @"Clear SSAO target";
    [encoder endEncoding];
  });
}

}  // namespace blue::render
