#ifndef BLUE_RENDER_D3D12_PASSES_FRAME_CONTEXT_H_
#define BLUE_RENDER_D3D12_PASSES_FRAME_CONTEXT_H_

#include <d3d12.h>

#include <span>

#include "core/extent.h"
#include "render/d3d12/frame_globals.h"
#include "render/d3d12/render_targets.h"
#include "render/draw_list.h"
#include "render/frame_plan.h"
#include "render/scene_view.h"

namespace blue::render::d3d12 {

// What every pass gets when it records its part of one frame (the
// counterpart of the Vulkan FrameContext). Built by D3d12Renderer::
// DrawFrameTo() and only valid during it; everything is borrowed.
struct FrameContext {
  ID3D12GraphicsCommandList* cmd = nullptr;
  const SceneView& view;
  // Which passes run this frame.
  const FramePlan& plan;
  // hdr, depth and ldr of the scene output, at targets.extent().
  RenderTargets& targets;
  // The frame set of this frame's main view.
  FrameBindings frame;
  // The visible submeshes, sorted (DrawList), for the camera passes.
  std::span<const DrawEntry> draw_list;
  RenderStats& stats;
};

// Sets a viewport and scissor covering `extent`. The scene passes need no
// flip: Direct3D's clip y points up, like the Vulkan backend's Y-up
// (negative height) scene viewport, so the same projection and winding
// (counter-clockwise front faces) apply.
inline void SetViewport(ID3D12GraphicsCommandList* cmd, core::Extent2D extent) {
  const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(extent.width),
                                static_cast<float>(extent.height), 0.0f, 1.0f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(extent.width),
                           static_cast<LONG>(extent.height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
}

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_FRAME_CONTEXT_H_
