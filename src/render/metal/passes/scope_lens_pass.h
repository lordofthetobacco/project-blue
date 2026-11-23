#ifndef BLUE_RENDER_METAL_PASSES_SCOPE_LENS_PASS_H_
#define BLUE_RENDER_METAL_PASSES_SCOPE_LENS_PASS_H_

#include "render/metal/metal_types.h"

#include <array>
#include <memory>
#include <string>

#include "render/metal/buffer.h"
#include "render/frame_config.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/pipeline.h"
#include "render/metal/shader_programs.h"

namespace blue::render::metal {

// Draws the eyepiece glass of SceneView::scope (shaders/scope_lens.*): a
// depth-tested disc on the weapon, alpha-blended by its antialiased rim,
// showing the scope's picture through the eyepiece's optics and reticle.
// Besides the frame's globals (the main view's) it binds its own lens
// uniforms (kBufferScopeLens, one buffer per frame in flight) and the
// picture (kTextureScopePicture).
//
// With the temporal scaler on (PostSettings::upscaling) the glass is drawn
// at the render resolution with the frame's view-projection, jittered like
// the scene whose depth it is tested against, and marked in the scaler's
// reactive mask (DrawReactive()). The picture itself is never jittered or
// scaled: it is a texture on the lens.
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

  // Records the glass's part of the temporal scaler's reactive mask, if the
  // view has a scope, on `encoder`: colour
  // RenderTargets::kReactiveMaskFormat (cleared to 0 by the caller), the
  // scene depth attachment and viewport, and the main view's globals set,
  // as for Draw(). Adds 1 over the glass (its antialiased rim as in
  // Draw()), depth tested like it. What the glass shows follows the weapon
  // and the aim, not the surface behind it whose motion vector its pixels
  // carry, so the scaler must ignore its history there. Call after Draw()
  // of the same frame (it uses the uniforms Draw() wrote); records nothing
  // before the first BindPicture().
  void DrawReactive(const FrameContext& ctx,
                    id<MTL4RenderCommandEncoder> encoder) const;

 private:
  // The lens uniforms of each frame in flight.
  std::array<Buffer, kFramesInFlight> uniforms_;
  id<MTLTexture> picture_ = nil;
  // Replaced when their shaders are reloaded; never null after
  // construction.
  std::unique_ptr<Pipeline> pipeline_;
  std::unique_ptr<Pipeline> reactive_pipeline_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_SCOPE_LENS_PASS_H_
