#ifndef BLUE_RENDER_D3D12_PASSES_OVERLAY_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_OVERLAY_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <vector>

#include "render/d3d12/buffer.h"
#include "render/d3d12/frame_globals.h"
#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/mesh.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"
#include "render/frame_config.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Samplers;

// Editor overlays drawn over the tonemapped image (ctx.targets.ldr()) after
// the tonemap pass and before the present pass, as the Vulkan OverlayPass:
//
//   lines      SceneView::lines (depth-tested against the scene depth, not
//              writing it) and SceneView::overlay_lines (always on top), one
//              pixel wide, alpha-blended (shaders/line.slang).
//   selection  The items of SceneView::selected_draws are rendered into
//              ctx.targets.selection_mask() (0.5 where hidden by the scene,
//              1 where visible), then an edge filter draws the outline in
//              SceneView::selection_color around the mask
//              (shaders/depth.slang, shaders/selection_outline.slang).
//
// Records nothing when the view has no lines and no selection. Line vertices
// go through a per-frame-in-flight upload buffer that only grows.
// States: expects ldr in PIXEL_SHADER_RESOURCE and the depth in
// RenderTargets::kDepthReadState and leaves them so; the selection mask is
// PIXEL_SHADER_RESOURCE between frames.
//
// Borrows everything it is given. Neither copyable nor movable. Not
// thread-safe.
class OverlayPass {
 public:
  // `scene_root` is a scene root signature (the line and mask programs read
  // the frame set only), `fullscreen_root` the full-screen one.
  OverlayPass(const D3d12Context& context, ShaderPrograms& programs,
              const RootSignature& scene_root,
              const RootSignature& fullscreen_root,
              const FrameGlobals& frame_globals, const Samplers& samplers,
              const std::vector<Mesh>& meshes);

  OverlayPass(const OverlayPass&) = delete;
  OverlayPass& operator=(const OverlayPass&) = delete;

  // Records the overlays of ctx.view (see the class comment); a no-op when
  // FramePlan::overlay is false.
  void Record(const FrameContext& ctx);

 private:
  // Copies the line vertices of ctx.view into this frame's buffer, growing it
  // if needed.
  void UploadLines(const FrameContext& ctx);
  void RecordLines(const FrameContext& ctx) const;
  void RecordSelectionMask(const FrameContext& ctx) const;
  // Binds `color` with the read-only scene depth and sets the scene root
  // signature, the frame set, the viewport and the scissor.
  void BeginOverlayDraws(const FrameContext& ctx,
                         D3D12_CPU_DESCRIPTOR_HANDLE color) const;

  const D3d12Context& context_;
  const RootSignature& scene_root_;
  SceneRootParams params_;
  const FrameGlobals& frame_globals_;
  const Samplers& samplers_;
  const std::vector<Mesh>& meshes_;
  std::array<Buffer, kFramesInFlight> line_buffers_;
  // Replaced on a shader reload.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> tested_lines_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> top_lines_;
  // Selection mask: hidden parts (no depth test) then visible parts (depth
  // test against the scene).
  Microsoft::WRL::ComPtr<ID3D12PipelineState> mask_hidden_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> mask_visible_;
  FullscreenPass outline_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_OVERLAY_PASS_H_
