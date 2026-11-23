#ifndef BLUE_RENDER_D3D12_PASSES_FRAME_CONTEXT_H_
#define BLUE_RENDER_D3D12_PASSES_FRAME_CONTEXT_H_

#include <d3d12.h>

#include <cstddef>
#include <span>

#include "camera/frustum.h"
#include "core/extent.h"
#include "lighting/cascades.h"
#include "render/d3d12/frame_globals.h"
#include "render/d3d12/image.h"
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
  size_t frame_index = 0;  // Frame in flight, [0, kFramesInFlight).
  const SceneView& view;
  // Which passes run this frame (the main view's plan, also for the scope
  // view).
  const FramePlan& plan;
  // hdr, depth and ldr of the scene output, at targets.extent().
  RenderTargets& targets;
  // What the camera passes (DepthPrepass, the scene pass and what is drawn
  // inside it) render into, from the top-left corner: targets.hdr(),
  // targets.depth() and targets.extent() for the main view; a scope's
  // picture for the scope view (D3d12Renderer::RecordScopeView()).
  Image& scene_hdr;
  Image& scene_depth;
  core::Extent2D scene_extent{};
  // The state EndScenePass() leaves scene_hdr in for whoever samples it.
  D3D12_RESOURCE_STATES scene_hdr_read_state =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  // The frame set of this view (the main view's or the scope view's).
  FrameBindings frame;
  // The visible submeshes, sorted (DrawList), for the camera passes.
  std::span<const DrawEntry> draw_list;
  // World-space bounds of view.draws (parallel to it), for the shadow
  // cascades' culling.
  std::span<const camera::Aabb> world_bounds;
  // The sun's shadow cascades; only meaningful when plan.shadows.
  std::span<const lighting::Cascade> cascades;
  RenderStats& stats;
};

// Sets a viewport and scissor covering `extent`. The scene passes need no
// flip: Direct3D's clip y points up, like the Vulkan backend's Y-up
// (negative height) scene viewport, so the same projection and winding
// (counter-clockwise front faces) apply.
inline void SetViewport(ID3D12GraphicsCommandList* cmd, core::Extent2D extent) {
  const D3D12_VIEWPORT viewport{0.0f,
                                0.0f,
                                static_cast<float>(extent.width),
                                static_cast<float>(extent.height),
                                0.0f,
                                1.0f};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(extent.width),
                           static_cast<LONG>(extent.height)};
  cmd->RSSetViewports(1, &viewport);
  cmd->RSSetScissorRects(1, &scissor);
}

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_FRAME_CONTEXT_H_
