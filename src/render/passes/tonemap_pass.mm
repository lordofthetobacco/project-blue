#include "render/passes/tonemap_pass.h"

#include <array>
#include <cmath>
#include <cstdint>

#include "core/path.h"

namespace blue::render {

namespace {

// Must match TonemapConstants in shaders/tonemap.frag.metal.
struct TonemapConstants {
  float exposure_scale;  // 2^EV
  int32_t tonemapper;    // Tonemapper
  float bloom_strength;  // 0 when bloom is off: the bloom image is not read
  float bloom_scale;     // 1 / bloom levels: mip 0 holds their sum
};
static_assert(sizeof(TonemapConstants) == 16);

}  // namespace

TonemapPass::TonemapPass(const MetalContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir)
    : pass_(context, programs, shader_dir,
            {.fragment_path = core::JoinPath(shader_dir, "tonemap.frag.metal"),
             .input_count = 2,
             .pass_constant_size = sizeof(TonemapConstants),
             .output_format = RenderTargets::kLdrFormat,
             .label = "tonemap"}) {}

void TonemapPass::BindTargets(const RenderTargets& targets,
                              const Samplers& samplers) {
  // The HDR image is fetched texel by texel; the half-resolution bloom is
  // upsampled bilinearly.
  const std::array<id<MTLTexture>, 2> inputs{targets.hdr().texture(),
                                             targets.bloom_mip_view(0)};
  pass_.SetInputs(0, inputs, samplers.linear_clamp());
}

void TonemapPass::Record(const FrameContext& ctx) const {
  const TonemapConstants constants{
      .exposure_scale = std::exp2(ctx.view.post.exposure_ev),
      .tonemapper = static_cast<int32_t>(ctx.view.post.tonemapper),
      .bloom_strength =
          ctx.view.post.bloom_enabled ? ctx.view.post.bloom_strength : 0.0f,
      .bloom_scale = 1.0f / static_cast<float>(ctx.targets.bloom_levels()),
  };
  pass_.Record(ctx.rec,
               {.texture = ctx.targets.ldr().texture(),
                .extent = ctx.targets.extent()},
               0, ConstantBytes(constants));
}

}  // namespace blue::render
