#ifndef BLUE_RENDER_D3D12_PASSES_MOTION_BLUR_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_MOTION_BLUR_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <optional>
#include <vector>

#include "render/d3d12/frame_globals.h"
#include "render/d3d12/fullscreen_pass.h"
#include "render/d3d12/mesh.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Samplers;

// Per-object motion blur of the draw items that have a previous_transform
// and DrawItem::motion_blur set (the first-person weapon), on the linear HDR
// image before bloom: the counterpart of the Vulkan MotionBlurPass, with the
// same programs, constants and steps, all restricted to `area` (FindArea()):
//
//   1. velocity   the moving items' draw-list entries -> velocity
//                 (shaders/motion_velocity.slang), depth tested EQUAL
//                 against the pre-pass like the mesh pass.
//   2. tile max   velocity -> motion_tiles (shaders/motion_tile_max.slang).
//   3. copy       hdr -> motion_source, the area only (the blur reads the
//                 image it writes).
//   4. blur       motion_source + velocity + motion_tiles -> hdr
//                 (shaders/motion_blur_gather.slang), scissored to the area.
//
// The velocity and tile images are written with the scene's Y-up
// convention, which Direct3D shares, and fetched by SV_Position: no flip.
// Borrows everything it is given. Neither copyable nor movable. Not
// thread-safe.
class MotionBlurPass {
 public:
  // `scene_root` is a scene root signature (the frame set is all the
  // velocity program reads), `fullscreen_root` the full-screen one.
  MotionBlurPass(const D3d12Context& context, ShaderPrograms& programs,
                 const RootSignature& scene_root,
                 const RootSignature& fullscreen_root,
                 const FrameGlobals& frame_globals, const Samplers& samplers,
                 const std::vector<Mesh>& meshes);

  MotionBlurPass(const MotionBlurPass&) = delete;
  MotionBlurPass& operator=(const MotionBlurPass&) = delete;

  // Returns the pixels the blur may change this frame, or nullopt when there
  // is nothing to do (as the Vulkan MotionBlurPass::FindArea()).
  [[nodiscard]] static std::optional<D3D12_RECT> FindArea(
      const FrameContext& ctx);

  // Records the four steps over `area` (from FindArea()). Expects hdr in
  // PIXEL_SHADER_RESOURCE after the scene pass and the depth in
  // RenderTargets::kDepthReadState; leaves hdr, velocity, motion_tiles and
  // motion_source in PIXEL_SHADER_RESOURCE.
  void Record(const FrameContext& ctx, const D3D12_RECT& area) const;

 private:
  void RecordVelocity(const FrameContext& ctx) const;
  void RecordTileMax(const FrameContext& ctx, const D3D12_RECT& area) const;
  // Copies `area` of hdr into motion_source and leaves hdr in RENDER_TARGET.
  static void RecordSourceCopy(const FrameContext& ctx, const D3D12_RECT& area);

  const D3d12Context& context_;
  const RootSignature& scene_root_;
  SceneRootParams params_;
  const FrameGlobals& frame_globals_;
  const Samplers& samplers_;
  const std::vector<Mesh>& meshes_;
  // Velocity pipelines, [0] culling back faces, [1] double-sided; replaced
  // on a shader reload.
  std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2>
      velocity_pipelines_;
  FullscreenPass tile_max_;
  FullscreenPass blur_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_MOTION_BLUR_PASS_H_
