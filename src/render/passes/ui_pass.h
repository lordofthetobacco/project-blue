#ifndef BLUE_RENDER_PASSES_UI_PASS_H_
#define BLUE_RENDER_PASSES_UI_PASS_H_

#include <imgui.h>

#include "render/passes/frame_context.h"

namespace blue::render {

// Draws the ImGui overlay `ui` (when non-null) over the swapchain image,
// which must be in COLOR_ATTACHMENT_OPTIMAL, without depth, then moves the
// image to PRESENT_SRC_KHR. The ImGui Vulkan backend must have been
// initialized for the swapchain format and no depth attachment.
void RecordUiPass(const FrameContext& ctx, ImDrawData* ui);

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_UI_PASS_H_
