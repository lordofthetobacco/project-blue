#ifndef BLUE_RENDER_PASSES_MOTION_VECTOR_PASS_H_
#define BLUE_RENDER_PASSES_MOTION_VECTOR_PASS_H_

#include "render/metal_types.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "render/fullscreen_pass.h"
#include "render/mesh.h"
#include "render/metal_context.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"

namespace blue::render {

// Writes the scene's motion vectors for the temporal scaler into
// RenderTargets::motion_vectors(): for every pixel, where its surface point
// was on screen in the previous frame, as the offset from where it is now in
// UV units of the target (MotionVector() in render/temporal_math.h; x right,
// y down). Never jittered: both positions come from unjittered
// view-projections (FrameUniforms::unjittered_view_proj /
// previous_view_proj). Two steps:
//
//   1. camera   depth -> motion_vectors (shaders/motion_camera.frag.metal):
//               one triangle finds each pixel's surface point again from
//               the depth pre-pass and projects it with both
//               view-projections. This is exact for everything that stands
//               still in the world and for the sky, and redraws no geometry.
//   2. items    the draw-list entries with a previous_transform ->
//               motion_vectors (shaders/motion_object.*): the items that
//               moved, drawn over it with their previous model matrix, depth
//               tested EQUAL against the pre-pass like the mesh pass, so
//               only their visible surface writes. Skipped (no encoder)
//               when nothing visible moved.
//
// What has no depth of its own keeps the vector of what is behind it: the
// sights, and alpha-masked cut-outs of a moved item (their depth is that of
// the background).
//
// Only recorded in frames with the temporal scaler (FrameContext::temporal),
// for the main view, after the depth pre-pass; otherwise it costs nothing.
// Borrows everything it is given, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class MotionVectorPass {
 public:
  MotionVectorPass(const MetalContext& context, ShaderPrograms& programs,
                   const std::string& shader_dir,
                   const std::vector<Mesh>& meshes);

  MotionVectorPass(const MotionVectorPass&) = delete;
  MotionVectorPass& operator=(const MotionVectorPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized. Does nothing while there is no motion vector target (the
  // temporal scaler is off).
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Records the two steps on ctx.rec, each in its own encoder. Reads the
  // scene depth the pre-pass wrote and leaves it as it was; writes every
  // pixel of the motion vector target.
  void Record(const FrameContext& ctx) const;

 private:
  // Step 2: draws the moved items over the camera motion.
  void RecordItems(const FrameContext& ctx) const;

  const std::vector<Mesh>& meshes_;
  FullscreenPass camera_;
  // Pipelines of the moved items, [0] culling back faces, [1] double-sided;
  // replaced when the shaders are reloaded, never null after construction.
  std::array<std::unique_ptr<Pipeline>, 2> item_pipelines_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_MOTION_VECTOR_PASS_H_
