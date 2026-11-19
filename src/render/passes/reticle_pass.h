#ifndef BLUE_RENDER_PASSES_RETICLE_PASS_H_
#define BLUE_RENDER_PASSES_RETICLE_PASS_H_

#include "render/metal_types.h"

#include <memory>
#include <string>

#include "render/metal_context.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/shader_programs.h"

namespace blue::render {

// Draws the reflex sight of SceneView::sight (shaders/reticle.*): an
// additive, depth-tested quad without a vertex buffer. Borrows `programs`,
// which must outlive it. Neither copyable nor movable. Not thread-safe.
class ReticlePass {
 public:
  ReticlePass(const MetalContext& context, ShaderPrograms& programs,
              const std::string& shader_dir);

  ReticlePass(const ReticlePass&) = delete;
  ReticlePass& operator=(const ReticlePass&) = delete;

  // Records the sight, if the view has one, on `encoder`, a scene encoder
  // (HDR colour, scene depth attachment) owned by the caller, which has set
  // the frame's globals on it (FrameGlobals::Bind()). Call after the opaque
  // meshes, so their depth hides the window.
  void Draw(const FrameContext& ctx, id<MTLRenderCommandEncoder> encoder) const;

 private:
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_RETICLE_PASS_H_
