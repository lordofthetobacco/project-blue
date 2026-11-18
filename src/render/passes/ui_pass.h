#ifndef BLUE_RENDER_PASSES_UI_PASS_H_
#define BLUE_RENDER_PASSES_UI_PASS_H_

#include <imgui.h>

#include "render/passes/frame_context.h"

namespace blue::render {

// What the UI pass draws over.
enum class UiBackground {
  // The present pass's output: the swapchain image is in
  // COLOR_ATTACHMENT_OPTIMAL and kept.
  kScene,
  // Nothing (the editor, whose scene is in a panel): the freshly acquired
  // swapchain image is cleared to black first.
  kClear,
};

// Draws the ImGui overlay `ui` (when non-null) over the swapchain image
// without depth, then moves the image to PRESENT_SRC_KHR. The ImGui Vulkan
// backend must have been initialized for the swapchain format and no depth
// attachment.
void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background);

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_UI_PASS_H_
