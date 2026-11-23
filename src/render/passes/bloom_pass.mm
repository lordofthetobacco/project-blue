#include "render/passes/bloom_pass.h"

#include <array>
#include <cstdint>

#include "core/path.h"

namespace blue::render {

namespace {

// Must match DownsampleConstants in shaders/bloom_down.frag.metal, where the
// float2 alignment pads the block to 16 bytes.
struct DownsampleConstants {
  float source_texel_x;
  float source_texel_y;
  int32_t karis_average;
  int32_t padding;
};
static_assert(sizeof(DownsampleConstants) == 16);

// Must match UpsampleConstants in shaders/bloom_up.frag.metal.
struct UpsampleConstants {
  float source_texel_x;
  float source_texel_y;
};
static_assert(sizeof(UpsampleConstants) == 8);

}  // namespace

BloomPass::BloomPass(const MetalContext& context, ShaderPrograms& programs,
                     const std::string& shader_dir)
    : downsample_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "bloom_down.frag.metal"),
           .input_set_count = RenderTargets::kMaxBloomLevels,
           .pass_constant_size = sizeof(DownsampleConstants),
           .output_format = RenderTargets::kBloomFormat,
           .label = "bloom downsample"}),
      upsample_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "bloom_up.frag.metal"),
           .input_set_count = RenderTargets::kMaxBloomLevels,
           .pass_constant_size = sizeof(UpsampleConstants),
           .output_format = RenderTargets::kBloomFormat,
           .blend = BlendMode::kAdditive,
           .label = "bloom upsample"}) {}

void BloomPass::BindTargets(const RenderTargets& targets,
                            const Samplers& samplers) {
  for (uint32_t level = 0; level < targets.bloom_levels(); ++level) {
    const std::array<id<MTLTexture>, 1> source{
        level == 0 ? targets.post_hdr().texture()
                   : targets.bloom_mip_view(level - 1)};
    downsample_.SetInputs(level, source, samplers.linear_clamp());
    if (level > 0) {
      const std::array<id<MTLTexture>, 1> smaller{
          targets.bloom_mip_view(level)};
      upsample_.SetInputs(level, smaller, samplers.linear_clamp());
    }
  }
}

void BloomPass::Record(const FrameContext& ctx) const {
  const RenderTargets& targets = ctx.targets;
  id<MTLTexture> bloom = targets.bloom().texture();
  const uint32_t levels = targets.bloom_levels();

  for (uint32_t level = 0; level < levels; ++level) {
    const core::Extent2D source =
        level == 0 ? targets.output_extent()
                   : targets.bloom_mip_extent(level - 1);
    const DownsampleConstants constants{
        .source_texel_x = 1.0f / static_cast<float>(source.width),
        .source_texel_y = 1.0f / static_cast<float>(source.height),
        .karis_average = level == 0 ? 1 : 0,
        .padding = 0,
    };
    downsample_.Record(ctx.rec,
                       {.texture = bloom,
                        .level = level,
                        .extent = targets.bloom_mip_extent(level)},
                       level, ConstantBytes(constants));
  }

  for (uint32_t level = levels - 1; level > 0; --level) {
    const uint32_t target = level - 1;
    const core::Extent2D source = targets.bloom_mip_extent(level);
    const UpsampleConstants constants{
        .source_texel_x = 1.0f / static_cast<float>(source.width),
        .source_texel_y = 1.0f / static_cast<float>(source.height),
    };
    // Loads the downsampled image: the upsample adds onto it.
    upsample_.Record(ctx.rec,
                     {.texture = bloom,
                      .level = target,
                      .extent = targets.bloom_mip_extent(target),
                      .load_action = MTLLoadActionLoad},
                     level, ConstantBytes(constants));
  }
}

}  // namespace blue::render
