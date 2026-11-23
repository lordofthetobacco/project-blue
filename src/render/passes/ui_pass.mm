#include "render/passes/ui_pass.h"

#include <imgui_impl_metal4.h>

#include "core/log.h"
#include "render/encoders.h"
#include "render/frame_config.h"

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

  MTL4RenderPassDescriptor* descriptor =
      [[MTL4RenderPassDescriptor alloc] init];
  MTLRenderPassColorAttachmentDescriptor* color =
      descriptor.colorAttachments[0];
  color.texture = target;
  color.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
  color.storeAction = MTLStoreActionStore;
  color.clearColor = clear_color;

  id<MTL4RenderCommandEncoder> encoder =
      BeginRenderPass(ctx.rec, descriptor, "ui");
  if (ui != nullptr) {
    // The backend's NewFrame does not take part in building the UI (the UI
    // layer already ran ImGui::NewFrame() and ImGui::Render()): it records
    // the attachment formats of `descriptor`, which select the pipeline of
    // RenderDrawData, and creates the backend's device objects on first use.
    // RenderDrawData uploads the font atlas (RendererHasTextures) into this
    // frame in flight's buffers, sets the backend's own argument table on
    // `encoder` (the renderer's is set again by the next BeginRenderPass())
    // and keeps what it draws resident in its own set on the queue.
    ImGui_ImplMetal4_NewFrame(descriptor, static_cast<int>(ctx.frame_index));
    ImGui_ImplMetal4_RenderDrawData(ui, ctx.rec.cmd, encoder);
  }
  [encoder endEncoding];
}

void UiRenderer::Init(const MetalContext& context) {
  if (!ImGui_ImplMetal4_Init(context.device(), context.queue(),
                             static_cast<int>(kFramesInFlight))) {
    core::log::Fatal("ImGui Metal 4 backend initialization failed");
  }
  initialized_ = true;
}

void UiRenderer::Shutdown() {
  if (!initialized_) {
    return;
  }
  ImGui_ImplMetal4_Shutdown();
  initialized_ = false;
}

}  // namespace blue::render
