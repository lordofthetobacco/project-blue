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
// additive, depth-tested quad without a vertex buffer. With the temporal
// scaler on (PostSettings::upscaling) it also marks the dot in the scaler's
// reactive mask, see DrawReactive(). The sight is drawn with the frame's
// view-projection, jittered like the scene whose depth it is tested against.
// Borrows `programs`, which must outlive it. Neither copyable nor movable.
// Not thread-safe.
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
  void Draw(const FrameContext& ctx,
            id<MTL4RenderCommandEncoder> encoder) const;

  // Records the sight's part of the temporal scaler's reactive mask, if the
  // view has a sight, on `encoder`: colour
  // RenderTargets::kReactiveMaskFormat (cleared to 0 by the caller), the
  // scene depth attachment and viewport, and the main view's globals set,
  // as for Draw(). Adds 1 where the dot is, depth tested like the sight;
  // the see-through rest of the window adds nothing, so what shows through
  // it keeps its history. The dot has no motion vector of its own (it is an
  // image at infinity that follows the weapon), so without the mask the
  // scaler would drag it along the surface behind the window.
  void DrawReactive(const FrameContext& ctx,
                    id<MTL4RenderCommandEncoder> encoder) const;

 private:
  // Replaced when their shaders are reloaded; never null after
  // construction.
  std::unique_ptr<Pipeline> pipeline_;
  std::unique_ptr<Pipeline> reactive_pipeline_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_RETICLE_PASS_H_
