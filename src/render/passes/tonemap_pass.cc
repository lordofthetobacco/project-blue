#include "render/passes/tonemap_pass.h"

#include <array>
#include <cmath>
#include <cstdint>

#include "core/path.h"
#include "render/barriers.h"

namespace blue::render {

namespace {

// Must match TonemapConstants in shaders/tonemap.frag.
struct TonemapConstants {
  float exposure_scale;  // 2^EV
  int32_t tonemapper;    // Tonemapper
  float bloom_strength;  // 0 when bloom is off: the bloom image is not read
  float bloom_scale;     // 1 / bloom levels: mip 0 holds their sum
};

}  // namespace

TonemapPass::TonemapPass(const VulkanContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir)
    : pass_(context, programs, shader_dir,
            {.fragment_path = core::JoinPath(shader_dir, "tonemap.frag"),
             .input_count = 2,
             .push_constant_size = sizeof(TonemapConstants),
             .output_format = RenderTargets::kLdrFormat}) {}

void TonemapPass::BindTargets(const RenderTargets& targets,
                              const Samplers& samplers) {
  // The HDR image is fetched texel by texel; the half-resolution bloom is
  // upsampled bilinearly.
  const std::array<VkImageView, 2> inputs{targets.hdr().view(),
                                          targets.bloom_mip_view(0)};
  pass_.SetInputs(0, inputs, samplers.linear_clamp());
}

void TonemapPass::Record(const FrameContext& ctx) const {
  const Image& ldr = ctx.targets.ldr();
  // The previous frame's present pass sampled the LDR image.
  ImageBarrier(ctx.cmd, ldr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  const TonemapConstants constants{
      .exposure_scale = std::exp2(ctx.view.post.exposure_ev),
      .tonemapper = static_cast<int32_t>(ctx.view.post.tonemapper),
      .bloom_strength =
          ctx.view.post.bloom_enabled ? ctx.view.post.bloom_strength : 0.0f,
      .bloom_scale = 1.0f / static_cast<float>(ctx.targets.bloom_levels()),
  };
  pass_.Record(ctx.cmd, {.view = ldr.view(), .extent = ctx.targets.extent()}, 0,
               PushBytes(constants));
  ImageBarrier(ctx.cmd, ldr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render
