#ifndef BLUE_RENDER_PASSES_FRAME_CONTEXT_H_
#define BLUE_RENDER_PASSES_FRAME_CONTEXT_H_

#include "render/metal_types.h"

#include <cstddef>
#include <span>

#include "camera/frustum.h"
#include "core/extent.h"
#include "lighting/cascades.h"
#include "render/draw_list.h"
#include "render/frame_globals.h"
#include "render/gpu_timer.h"
#include "render/image.h"
#include "render/render_targets.h"
#include "render/scene_view.h"

namespace blue::render {

// What every pass gets when it records its part of one frame. Built by
// RendererImpl::DrawFrameTo() and only valid during that frame's recording;
// everything is borrowed from the renderer.
struct FrameContext {
  // The frame's command buffer; passes open their encoders on it through
  // render/encoders.h.
  id<MTLCommandBuffer> cmd = nil;
  size_t frame_index = 0;  // Frame in flight, [0, kFramesInFlight).
  // FrameUniforms and the shared textures; a scene pass sets them on each
  // of its encoders with globals.Bind(encoder, frame_index, frame_view).
  const FrameGlobals& globals;
  // Whose uniforms this context renders with: the camera's, or a scope's
  // for the scope picture (RendererImpl::RecordScopeView()).
  FrameView frame_view = FrameView::kMain;
  const SceneView& view;
  const RenderTargets& targets;
  // What the camera passes (DepthPrepass, the scene pass and what is drawn
  // inside it) render into, from the top-left corner: targets.hdr(),
  // targets.depth() and targets.extent() for the main view; a scope's
  // picture for the scope view (RendererImpl::RecordScopeView()).
  const Image& scene_hdr;
  const Image& scene_depth;
  core::Extent2D scene_extent{};

  // The window's drawable this frame presents. nil until the renderer
  // acquires it, just before the first pass that renders to it, and stays
  // nil when none was available (those passes are skipped then).
  id<CAMetalDrawable> drawable = nil;
  // The size in pixels of the window output (the swapchain extent).
  core::Extent2D output_extent{};

  // Culling inputs: world-space bounds of view.draws (parallel to it) and
  // the camera frustum.
  std::span<const camera::Aabb> world_bounds;
  const camera::Frustum& camera_frustum;
  // The sun's shadow cascades; only meaningful when `shadows` is true.
  std::span<const lighting::Cascade> cascades;
  bool shadows = false;
  // The visible submeshes, sorted (DrawList), for the camera passes.
  std::span<const DrawEntry> draw_list;

  RenderStats& stats;
  // Encoders opened through render/encoders.h report here.
  GpuTimer& timer;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_FRAME_CONTEXT_H_
