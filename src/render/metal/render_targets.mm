#include "render/metal/render_targets.h"

#include <algorithm>
#include <cstdint>

#include "core/log.h"
#include "render/metal/encoders.h"
#include "render/motion_blur_math.h"
#include "render/texture_types.h"

namespace blue::render::metal {

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
  Resize(extent, extent, /*temporal=*/false);
}

core::Extent2D RenderTargets::bloom_mip_extent(uint32_t level) const {
  return {std::max(bloom_extent_.width >> level, 1u),
          std::max(bloom_extent_.height >> level, 1u)};
}

void RenderTargets::Resize(core::Extent2D output, core::Extent2D render,
                           bool temporal) {
  if (!temporal && render != output) {
    core::log::Fatal("Render targets: {} x {} cannot become {} x {} without "
                     "the temporal scaler",
                     render.width, render.height, output.width, output.height);
  }
  const auto half = [](core::Extent2D extent) {
    return core::Extent2D{std::max((extent.width + 1) / 2, 1u),
                          std::max((extent.height + 1) / 2, 1u)};
  };
  output_extent_ = output;
  render_extent_ = render;
  ssao_extent_ = half(render);
  bloom_extent_ = half(output);
  temporal_ = temporal;

  hdr_ = MakeTarget(context_, render, kHdrFormat, "HDR target");
  depth_ = MakeTarget(context_, render, kDepthFormat, "Depth target");
  ldr_ = MakeTarget(context_, output, kLdrFormat, "LDR target");
  ssao_raw_ =
      MakeTarget(context_, ssao_extent_, kSsaoRawFormat, "SSAO raw target");
  ssao_blur_ =
      MakeTarget(context_, ssao_extent_, kSsaoRawFormat, "SSAO blur target");
  ssao_ = MakeTarget(context_, ssao_extent_, kSsaoFormat, "SSAO target");
  selection_mask_ = MakeTarget(context_, output, kSelectionMaskFormat,
                               "Selection mask target");

  motion_tile_size_ = MotionBlurMaxRadius(render.height);
  const auto tile_size = static_cast<uint32_t>(motion_tile_size_);
  velocity_ = MakeTarget(context_, render, kVelocityFormat, "Velocity target");
  motion_tiles_ = MakeTarget(context_,
                             {(render.width + tile_size - 1) / tile_size,
                              (render.height + tile_size - 1) / tile_size},
                             kMotionTileFormat, "Motion tiles target");
  motion_source_ =
      MakeTarget(context_, render, kHdrFormat, "Motion blur source");

  motion_vectors_ = {};
  reactive_mask_ = {};
  upscaled_ = {};
  if (temporal) {
    motion_vectors_ = MakeTarget(context_, render, kMotionVectorFormat,
                                 "Motion vector target");
    reactive_mask_ = MakeTarget(context_, render, kReactiveMaskFormat,
                                "Reactive mask target");
    // The scaler writes its output from a compute stage.
    upscaled_ = Image(context_, {.extent = output,
                                 .format = kHdrFormat,
                                 .usage = kSampledTarget |
                                          MTLTextureUsageShaderWrite,
                                 .label = "Upscaled HDR target"});
  }

  bloom_mip_views_.clear();
  const uint32_t levels =
      std::min(kMaxBloomLevels, MipLevelCount(bloom_extent_));
  bloom_ = MakeTarget(context_, bloom_extent_, kBloomFormat, "Bloom target",
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

}  // namespace blue::render::metal
