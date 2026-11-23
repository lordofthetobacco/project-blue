#ifndef BLUE_RENDER_METAL_PASSES_PRESENT_PASS_H_
#define BLUE_RENDER_METAL_PASSES_PRESENT_PASS_H_

#include "render/metal/metal_types.h"

#include <string>

#include "core/extent.h"
#include "render/metal/fullscreen_pass.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/render_targets.h"
#include "render/metal/samplers.h"
#include "render/metal/shader_programs.h"

namespace blue::render::metal {

// The texture a PresentPass writes: the drawable's texture or the editor
// viewport image. Its previous contents are discarded.
struct PresentTarget {
  id<MTLTexture> texture = nil;
  core::Extent2D extent{};
};

// Writes the LDR target to an output texture (shaders/present.frag.metal),
// through FXAA when SceneView::post.fxaa_enabled (using the luma the tonemap
// pass stored in alpha), else as a plain copy. Borrows `context` and
// `programs`, which must outlive it. Neither copyable nor movable. Not
// thread-safe.
class PresentPass {
 public:
  // `output_format` is the format of every texture this pass will draw to;
  // an _sRGB format makes the pass decode the LDR image first.
  PresentPass(const MetalContext& context, ShaderPrograms& programs,
              const std::string& shader_dir, MTLPixelFormat output_format);

  PresentPass(const PresentPass&) = delete;
  PresentPass& operator=(const PresentPass&) = delete;

  // Points the pass at the current targets; call after they were created or
  // resized.
  void BindTargets(const RenderTargets& targets, const Samplers& samplers);

  // Reads the LDR target and writes `target`.
  void Record(const FrameContext& ctx, const PresentTarget& target) const;

 private:
  bool srgb_output_;
  FullscreenPass pass_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_PRESENT_PASS_H_
