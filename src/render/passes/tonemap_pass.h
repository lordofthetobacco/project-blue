#ifndef BLUE_RENDER_PASSES_TONEMAP_PASS_H_
#define BLUE_RENDER_PASSES_TONEMAP_PASS_H_

#include "render/metal_types.h"

#include <string>

#include "render/fullscreen_pass.h"
#include "render/metal_context.h"
#include "render/passes/frame_context.h"
#include "render/render_targets.h"
#include "render/samplers.h"
#include "render/shader_programs.h"

namespace blue::render {

// Bloom mix, exposure and tonemapping (shaders/tonemap.frag.metal): HDR
// target (+ bloom mip 0) -> LDR target (sRGB-encoded colour, luma in alpha),
// with SceneView::post. Borrows `context` and `programs`, which must outlive
// it. Neither copyable nor movable. Not thread-safe.
class TonemapPass {
 public:
  TonemapPass(const MetalContext& context, ShaderPrograms& programs,
              const std::string& shader_dir);

  TonemapPass(const TonemapPass&) = delete;
  TonemapPass& operator=(const TonemapPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Reads the HDR target and, only when bloom is on, bloom mip 0; writes the
  // LDR target.
  void Record(const FrameContext& ctx) const;

 private:
  FullscreenPass pass_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_TONEMAP_PASS_H_
