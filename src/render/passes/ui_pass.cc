#include "render/passes/ui_pass.h"

#include <imgui_impl_vulkan.h>

#include "render/barriers.h"

namespace blue::render {

void RecordUiPass(const FrameContext& ctx, ImDrawData* ui) {
  if (ui != nullptr) {
    const VkRenderingAttachmentInfo color_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ctx.swapchain_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, ctx.swapchain_extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
    };
    // Rendering scopes do not order attachment accesses between them.
    ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
                 kColorAttachmentWrite, kColorAttachmentReadWrite,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkCmdBeginRendering(ctx.cmd, &rendering_info);
    ImGui_ImplVulkan_RenderDrawData(ui, ctx.cmd);
    vkCmdEndRendering(ctx.cmd);
  }
  ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, kNoAccess,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

}  // namespace blue::render
