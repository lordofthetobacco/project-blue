#include "render/passes/ui_pass.h"

#include <imgui_impl_metal.h>

#include "core/log.h"
#include "render/encoders.h"

namespace blue::render {

namespace {

// Returns whether `ui`, laid out for DisplaySize * FramebufferScale pixels
// (truncated, as the backend computes them), fits into `target`. The backend
// clamps its scissor rects to that size, not to the attachment's, and Metal
// rejects a scissor rect beyond the attachment.
bool UiFits(const ImDrawData& ui, id<MTLTexture> target) {
  const auto width = static_cast<int>(ui.DisplaySize.x * ui.FramebufferScale.x);
  const auto height =
      static_cast<int>(ui.DisplaySize.y * ui.FramebufferScale.y);
  return width <= static_cast<int>(target.width) &&
         height <= static_cast<int>(target.height);
}

}  // namespace

void RecordUiPass(const FrameContext& ctx, ImDrawData* ui,
                  UiBackground background, MTLClearColor clear_color) {
  const bool clear = background == UiBackground::kClear;
  id<MTLTexture> target = ctx.drawable.texture;
  if (ui != nullptr && !UiFits(*ui, target)) {
    ui = nullptr;  // One frame without UI while a resize settles.
  }
  if (ui == nullptr && !clear) {
    return;  // Nothing to draw, nothing to clear.
  }

  MTLRenderPassDescriptor* descriptor =
      [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor* color =
      descriptor.colorAttachments[0];
  color.texture = target;
  color.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
  color.storeAction = MTLStoreActionStore;
  color.clearColor = clear_color;

  id<MTLRenderCommandEncoder> encoder = BeginRenderPass(ctx, descriptor, "ui");
  if (ui != nullptr) {
    // The backend's NewFrame does not take part in building the UI (the UI
    // layer already ran ImGui::NewFrame() and ImGui::Render()): it records
    // the attachment formats of `descriptor`, which select the pipeline of
    // RenderDrawData, and creates the backend's device objects on first use.
    // RenderDrawData uploads the font atlas (RendererHasTextures).
    ImGui_ImplMetal_NewFrame(descriptor);
    ImGui_ImplMetal_RenderDrawData(ui, ctx.cmd, encoder);
  }
  [encoder endEncoding];
}

void UiRenderer::Init(const MetalContext& context) {
  if (!ImGui_ImplMetal_Init(context.device())) {
    core::log::Fatal("ImGui Metal backend initialization failed");
  }
  initialized_ = true;
}

void UiRenderer::Shutdown() {
  if (!initialized_) {
    return;
  }
  ImGui_ImplMetal_Shutdown();
  initialized_ = false;
}

}  // namespace blue::render
