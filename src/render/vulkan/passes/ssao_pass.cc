#include "render/vulkan/passes/ssao_pass.h"

#include <array>
#include <cstdint>

#include <glm/vec4.hpp>

#include "core/path.h"
#include "render/vulkan/barriers.h"

namespace blue::render::vulkan {

namespace {

// Must match SsaoConstants in shaders/ssao.frag.
struct SsaoConstants {
  glm::vec4 projection;  // x: proj[0][0], y: proj[1][1], z: near plane
  glm::vec4 params;      // x: radius, y: intensity, z: max radius (pixels)
  glm::vec4 full_size;   // xy: full-resolution size, zw: reciprocal
};

// Must match BlurConstants in shaders/ssao_blur.frag.
struct BlurConstants {
  int32_t axis_x;
  int32_t axis_y;
};

// Moves `image` (whose previous contents are not needed) into a color
// attachment, after whatever sampled it last.
void BeginWriting(VkCommandBuffer cmd, const Image& image) {
  ImageBarrier(cmd, image.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

// Makes what was written to `image` readable by fragment shaders.
void EndWriting(VkCommandBuffer cmd, const Image& image) {
  ImageBarrier(cmd, image.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace

SsaoPass::SsaoPass(const VulkanContext& context, ShaderPrograms& programs,
                   const std::string& shader_dir)
    : occlusion_(context, programs, shader_dir,
                 {.fragment_path = core::JoinPath(shader_dir, "ssao.frag"),
                  .push_constant_size = sizeof(SsaoConstants),
                  .output_format = RenderTargets::kSsaoRawFormat}),
      blur_horizontal_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "ssao_blur.frag"),
           .push_constant_size = sizeof(BlurConstants),
           .output_format = RenderTargets::kSsaoRawFormat}),
      blur_vertical_(
          context, programs, shader_dir,
          {.fragment_path = core::JoinPath(shader_dir, "ssao_blur.frag"),
           .push_constant_size = sizeof(BlurConstants),
           .output_format = RenderTargets::kSsaoFormat}) {}

void SsaoPass::BindTargets(const RenderTargets& targets,
                           const Samplers& samplers) {
  const std::array<VkImageView, 1> depth{targets.depth().view()};
  occlusion_.SetInputs(0, depth, samplers.nearest_clamp(),
                       VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL);
  const std::array<VkImageView, 1> raw{targets.ssao_raw().view()};
  blur_horizontal_.SetInputs(0, raw, samplers.nearest_clamp());
  const std::array<VkImageView, 1> blurred{targets.ssao_blur().view()};
  blur_vertical_.SetInputs(0, blurred, samplers.nearest_clamp());
  target_white_ = true;  // RenderTargets::Resize clears it.
}

void SsaoPass::Record(const FrameContext& ctx) {
  if (ctx.plan.ssao) {
    RecordOcclusion(ctx);
    target_white_ = false;
  } else if (!target_white_) {
    RecordClearToWhite(ctx);
    target_white_ = true;
  }
}

void SsaoPass::RecordOcclusion(const FrameContext& ctx) const {
  const VkCommandBuffer cmd = ctx.cmd;
  const RenderTargets& targets = ctx.targets;
  const VkExtent2D full = targets.extent();
  const VkExtent2D half = targets.half_extent();
  const glm::mat4& projection = ctx.view.camera.projection;
  const auto full_width = static_cast<float>(full.width);
  const auto full_height = static_cast<float>(full.height);

  const SsaoConstants constants{
      .projection =
          glm::vec4(projection[0][0], projection[1][1], projection[3][2], 0.0f),
      .params =
          glm::vec4(ctx.view.post.ssao_radius, ctx.view.post.ssao_intensity,
                    kMaxRadiusScreenFraction * full_height, 0.0f),
      .full_size = glm::vec4(full_width, full_height, 1.0f / full_width,
                             1.0f / full_height),
  };
  BeginWriting(cmd, targets.ssao_raw());
  occlusion_.Record(cmd, {.view = targets.ssao_raw().view(), .extent = half}, 0,
                    PushBytes(constants));
  EndWriting(cmd, targets.ssao_raw());

  const BlurConstants horizontal{.axis_x = 1, .axis_y = 0};
  BeginWriting(cmd, targets.ssao_blur());
  blur_horizontal_.Record(cmd,
                          {.view = targets.ssao_blur().view(), .extent = half},
                          0, PushBytes(horizontal));
  EndWriting(cmd, targets.ssao_blur());

  // The previous frame's mesh pass sampled the final target.
  const BlurConstants vertical{.axis_x = 0, .axis_y = 1};
  BeginWriting(cmd, targets.ssao());
  blur_vertical_.Record(cmd, {.view = targets.ssao().view(), .extent = half}, 0,
                        PushBytes(vertical));
  EndWriting(cmd, targets.ssao());
}

void SsaoPass::RecordClearToWhite(const FrameContext& ctx) {
  constexpr BarrierScope kClearWrite{VK_PIPELINE_STAGE_2_CLEAR_BIT,
                                     VK_ACCESS_2_TRANSFER_WRITE_BIT};
  const VkImage image = ctx.targets.ssao().handle();
  ImageBarrier(ctx.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, kFragmentSampledRead,
               kClearWrite, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  const VkClearColorValue white{{1.0f, 1.0f, 1.0f, 1.0f}};
  const VkImageSubresourceRange range = WholeImage(VK_IMAGE_ASPECT_COLOR_BIT);
  vkCmdClearColorImage(ctx.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       &white, 1, &range);
  ImageBarrier(ctx.cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, kClearWrite,
               kFragmentSampledRead, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render::vulkan
