#include "render/passes/bloom_pass.h"

#include <array>
#include <cstdint>

#include "core/path.h"
#include "render/barriers.h"

namespace blue::render {

namespace {

// Must match DownsampleConstants in shaders/bloom_down.frag.
struct DownsampleConstants {
  float source_texel_x;
  float source_texel_y;
  int32_t karis_average;
};

// Must match UpsampleConstants in shaders/bloom_up.frag.
struct UpsampleConstants {
  float source_texel_x;
  float source_texel_y;
};

// The single-mip range of bloom mip `level`.
VkImageSubresourceRange MipRange(uint32_t level) {
  return {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
}

}  // namespace

BloomPass::BloomPass(const VulkanContext& context, ShaderPrograms& programs,
                     const std::string& shader_dir)
    : downsample_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "bloom_down.frag"),
           .input_set_count = RenderTargets::kMaxBloomLevels,
           .push_constant_size = sizeof(DownsampleConstants),
           .output_format = RenderTargets::kBloomFormat}),
      upsample_(context, programs, shader_dir,
                {.fragment_path = core::JoinPath(shader_dir, "bloom_up.frag"),
                 .input_set_count = RenderTargets::kMaxBloomLevels,
                 .push_constant_size = sizeof(UpsampleConstants),
                 .output_format = RenderTargets::kBloomFormat,
                 .blend = BlendMode::kAdditive}) {}

void BloomPass::BindTargets(const RenderTargets& targets,
                            const Samplers& samplers) {
  for (uint32_t level = 0; level < targets.bloom_levels(); ++level) {
    const std::array<VkImageView, 1> source{
        level == 0 ? targets.hdr().view() : targets.bloom_mip_view(level - 1)};
    downsample_.SetInputs(level, source, samplers.linear_clamp());
    if (level > 0) {
      const std::array<VkImageView, 1> smaller{targets.bloom_mip_view(level)};
      upsample_.SetInputs(level, smaller, samplers.linear_clamp());
    }
  }
}

void BloomPass::Record(const FrameContext& ctx) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const RenderTargets& targets = ctx.targets;
  const VkImage bloom = targets.bloom().handle();
  const uint32_t levels = targets.bloom_levels();

  for (uint32_t level = 0; level < levels; ++level) {
    // Read last by the previous frame's tonemap pass (mip 0) or by this
    // chain's upsample of the previous frame.
    ImageBarrier(cmd, bloom, MipRange(level), kFragmentSampledRead,
                 kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkExtent2D source =
        level == 0 ? targets.extent() : targets.bloom_mip_extent(level - 1);
    const DownsampleConstants constants{
        .source_texel_x = 1.0f / static_cast<float>(source.width),
        .source_texel_y = 1.0f / static_cast<float>(source.height),
        .karis_average = level == 0 ? 1 : 0,
    };
    downsample_.Record(cmd,
                       {.view = targets.bloom_mip_view(level),
                        .extent = targets.bloom_mip_extent(level)},
                       level, PushBytes(constants));
    ImageBarrier(cmd, bloom, MipRange(level), kColorAttachmentWrite,
                 kFragmentSampledRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  for (uint32_t level = levels - 1; level > 0; --level) {
    const uint32_t target = level - 1;
    // Keeps the downsampled image: the upsample adds onto it.
    ImageBarrier(cmd, bloom, MipRange(target), kFragmentSampledRead,
                 kColorAttachmentReadWrite,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkExtent2D source = targets.bloom_mip_extent(level);
    const UpsampleConstants constants{
        .source_texel_x = 1.0f / static_cast<float>(source.width),
        .source_texel_y = 1.0f / static_cast<float>(source.height),
    };
    upsample_.Record(cmd,
                     {.view = targets.bloom_mip_view(target),
                      .extent = targets.bloom_mip_extent(target),
                      .load_op = VK_ATTACHMENT_LOAD_OP_LOAD},
                     level, PushBytes(constants));
    ImageBarrier(cmd, bloom, MipRange(target), kColorAttachmentReadWrite,
                 kFragmentSampledRead, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }
}

}  // namespace blue::render
