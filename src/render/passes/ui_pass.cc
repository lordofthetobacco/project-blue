#include "render/passes/ui_pass.h"

#include <imgui_impl_vulkan.h>

#include "render/barriers.h"

namespace blue::render {

void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background, const UiOutputUse& use) {
  const bool clear = background == UiBackground::kClear;
  if (clear) {
    ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
                 use.previous_use, kColorAttachmentWrite,
                 VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  } else if (ui != nullptr) {
    // Rendering scopes do not order attachment accesses between them.
    ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
                 kColorAttachmentWrite, kColorAttachmentReadWrite,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }
  if (ui != nullptr || clear) {
    const VkRenderingAttachmentInfo color_attachment{
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ctx.swapchain_view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp =
            clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}},
    };
    const VkRenderingInfo rendering_info{
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, ctx.swapchain_extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment,
    };
    vkCmdBeginRendering(ctx.cmd, &rendering_info);
    if (ui != nullptr) {
      ImGui_ImplVulkan_RenderDrawData(ui, ctx.cmd);
    }
    vkCmdEndRendering(ctx.cmd);
  }
  ImageBarrier(ctx.cmd, ctx.swapchain_image, VK_IMAGE_ASPECT_COLOR_BIT,
               kColorAttachmentWrite, use.next_use,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, use.final_layout);
}

}  // namespace blue::render
