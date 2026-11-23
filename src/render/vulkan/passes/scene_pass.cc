#include "render/vulkan/passes/scene_pass.h"

#include "render/vulkan/barriers.h"

namespace blue::render::vulkan {

namespace {

// Linear radiance where nothing (not even the sky) is drawn.
constexpr VkClearColorValue kClearColor{{0.015f, 0.018f, 0.022f, 1.0f}};

// Begins rendering into the HDR target (in COLOR_ATTACHMENT_OPTIMAL),
// cleared or loaded per `load_op`, with the read-only scene depth, and sets
// the Y-up viewport and scissor.
void BeginHdrRendering(const FrameContext& ctx, VkAttachmentLoadOp load_op) {
  const VkCommandBuffer cmd = ctx.cmd;
  const VkExtent2D extent = ctx.scene_extent;
  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ctx.scene_hdr.view(),
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = load_op,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = kClearColor},
  };
  const VkRenderingAttachmentInfo depth_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ctx.scene_depth.view(),
      .imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
      .storeOp = VK_ATTACHMENT_STORE_OP_NONE,
  };
  const VkRenderingInfo rendering_info{
      .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
      .renderArea = {{0, 0}, extent},
      .layerCount = 1,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_attachment,
      .pDepthAttachment = &depth_attachment,
  };
  vkCmdBeginRendering(cmd, &rendering_info);

  const VkViewport viewport{
      .x = 0.0f,
      .y = static_cast<float>(extent.height),
      .width = static_cast<float>(extent.width),
      .height = -static_cast<float>(extent.height),
      .minDepth = 0.0f,
      .maxDepth = 1.0f,
  };
  const VkRect2D scissor{{0, 0}, extent};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
}

}  // namespace

void BeginScenePass(const FrameContext& ctx) {
  // The previous frame's post chain sampled the HDR image. The depth target
  // needs no barrier: the pre-pass left it read-only for this pass.
  ImageBarrier(ctx.cmd, ctx.scene_hdr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  BeginHdrRendering(ctx, VK_ATTACHMENT_LOAD_OP_CLEAR);
}

void ResumeScenePass(const FrameContext& ctx) {
  // Written last as a color attachment (the scene or a full-screen pass),
  // then possibly made readable by fragment shaders.
  constexpr BarrierScope kHdrWritten{
      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
  ImageBarrier(ctx.cmd, ctx.scene_hdr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kHdrWritten, kColorAttachmentReadWrite,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  BeginHdrRendering(ctx, VK_ATTACHMENT_LOAD_OP_LOAD);
}

void EndScenePass(const FrameContext& ctx) {
  vkCmdEndRendering(ctx.cmd);
  ImageBarrier(ctx.cmd, ctx.scene_hdr.handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render::vulkan
