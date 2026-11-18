#include "render/passes/present_pass.h"

#include <array>
#include <cstdint>

#include "core/path.h"
#include "render/barriers.h"

namespace blue::render {

namespace {

// Must match PresentConstants in shaders/present.frag.
struct PresentConstants {
  int32_t decode_srgb;  // 1: the target encodes, so output linear values.
  int32_t fxaa;         // 1: anti-alias.
  float texel_width;    // 1 / image size
  float texel_height;
};

bool IsSrgb(VkFormat format) {
  return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

}  // namespace

PresentPass::PresentPass(const VulkanContext& context, ShaderPrograms& programs,
                         const std::string& shader_dir,
                         VkFormat swapchain_format)
    : srgb_swapchain_(IsSrgb(swapchain_format)),
      pass_(context, programs, shader_dir,
            {.fragment_path = core::JoinPath(shader_dir, "present.frag"),
             .input_count = 1,
             .push_constant_size = sizeof(PresentConstants),
             .output_format = swapchain_format}) {}

void PresentPass::BindTargets(const RenderTargets& targets,
                              const Samplers& samplers) {
  const std::array<VkImageView, 1> inputs{targets.ldr().view()};
  // FXAA needs bilinear taps; the plain copy uses texelFetch.
  pass_.SetInputs(0, inputs, samplers.linear_clamp());
}

void PresentPass::Record(const FrameContext& ctx) const {
  // Nothing to wait for but the acquire semaphore, which the submission
  // waits for at the color attachment output stage.
  ImageBarrier(
      ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
      {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_NONE},
      kColorAttachmentWrite, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  const VkExtent2D extent = ctx.targets.extent();
  const PresentConstants constants{
      .decode_srgb = srgb_swapchain_ ? 1 : 0,
      .fxaa = ctx.view.post.fxaa_enabled ? 1 : 0,
      .texel_width = 1.0f / static_cast<float>(extent.width),
      .texel_height = 1.0f / static_cast<float>(extent.height),
  };
  pass_.Record(ctx.cmd,
               {.view = ctx.swapchain_view, .extent = ctx.swapchain_extent}, 0,
               PushBytes(constants));
}

}  // namespace blue::render
