#ifndef BLUE_RENDER_PASSES_SCOPE_LENS_PASS_H_
#define BLUE_RENDER_PASSES_SCOPE_LENS_PASS_H_

#include "render/metal_types.h"

#include <array>
#include <memory>
#include <string>

#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/metal_context.h"
#include "render/passes/frame_context.h"
#include "render/pipeline.h"
#include "render/shader_programs.h"

namespace blue::render {

// Draws the eyepiece glass of SceneView::scope (shaders/scope_lens.*): a
// depth-tested disc on the weapon, alpha-blended by its antialiased rim,
// showing the scope's picture through the eyepiece's optics and reticle.
// Besides the frame's globals (the main view's) it binds its own lens
// uniforms (kBufferScopeLens, one buffer per frame in flight) and the
// picture (kTextureScopePicture).
//
// Borrows `programs`, which must outlive it. Neither copyable nor movable.
// Not thread-safe.
class ScopeLensPass {
 public:
  ScopeLensPass(const MetalContext& context, ShaderPrograms& programs,
                const std::string& shader_dir);

  ScopeLensPass(const ScopeLensPass&) = delete;
  ScopeLensPass& operator=(const ScopeLensPass&) = delete;

  // Sets the scope picture the eyepiece shows: `picture`, which must not be
  // nil, is sampled with the frame's bilinear clamped sampler
  // (kSamplerLinearClamp) and retained; its owner keeps it resident and
  // alive while a frame in flight may draw it. Draw() records nothing before
  // the first call.
  void BindPicture(id<MTLTexture> picture);

  // Records the eyepiece, if the view has a scope, on `encoder`, a scene
  // encoder (HDR colour, scene depth attachment) owned by the caller, which
  // has set the main view's globals on it (FrameGlobals::Bind()). Call after
  // the opaque meshes, so their depth hides the glass, and at most once per
  // frame in flight (the uniforms of ctx.frame_index are overwritten); the
  // picture must not be an attachment of `encoder`.
  void Draw(const FrameContext& ctx,
            id<MTL4RenderCommandEncoder> encoder) const;

 private:
  // The lens uniforms of each frame in flight.
  std::array<Buffer, kFramesInFlight> uniforms_;
  id<MTLTexture> picture_ = nil;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PASSES_SCOPE_LENS_PASS_H_
