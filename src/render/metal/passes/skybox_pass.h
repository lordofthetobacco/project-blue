#ifndef BLUE_RENDER_METAL_PASSES_SKYBOX_PASS_H_
#define BLUE_RENDER_METAL_PASSES_SKYBOX_PASS_H_

#include "render/metal/metal_types.h"

#include <memory>
#include <string>

#include "render/metal/environment.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/pipeline.h"
#include "render/metal/shader_programs.h"

namespace blue::render::metal {

// Draws the environment's sky cube behind the scene (shaders/skybox.*): a
// full-screen triangle at reversed-Z depth 0 that only covers pixels the
// opaque meshes left at the cleared depth (GreaterEqual, no depth write).
// Borrows `programs` and `environment`, which must outlive it; the
// environment's sky texture never changes. Neither copyable nor movable.
// Not thread-safe.
class SkyboxPass {
 public:
  SkyboxPass(const MetalContext& context, ShaderPrograms& programs,
             const std::string& shader_dir, const Environment& environment);

  SkyboxPass(const SkyboxPass&) = delete;
  SkyboxPass& operator=(const SkyboxPass&) = delete;

  // Records the sky on `encoder`, a scene encoder (HDR colour, scene depth
  // attachment) owned by the caller, which has bound the frame's globals
  // (FrameGlobals::Bind()). Call after the opaque meshes.
  void Draw(const FrameContext& ctx,
            id<MTL4RenderCommandEncoder> encoder) const;

 private:
  const Environment& environment_;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_SKYBOX_PASS_H_
