#ifndef BLUE_RENDER_PASSES_UI_PASS_H_
#define BLUE_RENDER_PASSES_UI_PASS_H_

#include <imgui.h>
#include <vulkan/vulkan.h>

#include "render/barriers.h"

#include "render/passes/frame_context.h"

namespace blue::render {

// What the UI pass draws over.
enum class UiBackground {
  // The present pass's output: the swapchain image is in
  // COLOR_ATTACHMENT_OPTIMAL and kept.
  kScene,
  // Nothing (the editor, whose scene is in a panel): the output image is
  // cleared to black first.
  kClear,
};

// How the output image is used around the UI pass.
struct UiOutputUse {
  // The last access before the pass, which a kClear transition waits for
  // (a freshly acquired swapchain image: nothing beyond the acquire
  // semaphore, waited for at the color attachment output stage).
  BarrierScope previous_use{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_ACCESS_2_NONE};
  // The layout the pass leaves the image in, and the next access, which
  // that transition makes wait (nothing for presenting: the submission's
  // semaphore orders it).
  VkImageLayout final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  BarrierScope next_use;
};

// The window: the swapchain image is presented.
inline constexpr UiOutputUse kUiPresent{};
// Headless: the offscreen output image is read by copies (readbacks), also
// in the previous frame.
inline constexpr UiOutputUse kUiCopy{
    .previous_use = kCopySourceRead,
    .final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .next_use = kCopySourceRead,
};

// Draws the ImGui overlay `ui` (when non-null) over the output image
// (FrameContext::swapchain_image: the swapchain image, or the offscreen
// output image when headless) without depth, then moves the image as
// `use` says. The ImGui Vulkan backend must have been initialized for
// the output format and no depth attachment.
void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background, const UiOutputUse& use);

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_UI_PASS_H_
