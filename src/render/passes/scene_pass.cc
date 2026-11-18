#include "render/passes/scene_pass.h"

#include "render/barriers.h"

namespace blue::render {

namespace {

// Linear radiance where nothing (not even the sky) is drawn.
constexpr VkClearColorValue kClearColor{{0.015f, 0.018f, 0.022f, 1.0f}};

}  // namespace

void BeginScenePass(const FrameContext& ctx) {
  const VkCommandBuffer cmd = ctx.cmd;
  // The previous frame's post chain sampled the HDR image. The depth target
  // needs no barrier: the pre-pass left it read-only for this pass.
  ImageBarrier(cmd, ctx.targets.hdr().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kFragmentSampledRead, kColorAttachmentWrite,
               VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

  const VkExtent2D extent = ctx.targets.extent();
  const VkRenderingAttachmentInfo color_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ctx.targets.hdr().view(),
      .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .clearValue = {.color = kClearColor},
  };
  const VkRenderingAttachmentInfo depth_attachment{
      .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
      .imageView = ctx.targets.depth().view(),
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

void EndScenePass(const FrameContext& ctx) {
  vkCmdEndRendering(ctx.cmd);
  ImageBarrier(ctx.cmd, ctx.targets.hdr().handle(), VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kFragmentSampledRead,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

}  // namespace blue::render
