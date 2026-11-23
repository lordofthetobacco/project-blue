#ifndef BLUE_RENDER_D3D12_PASSES_SCOPE_LENS_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_SCOPE_LENS_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "render/d3d12/frame_globals.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Image;
class Samplers;
class UploadRing;

// Draws the eyepiece glass of SceneView::scope (shaders/scope_lens.slang): a
// depth-tested disc on the weapon, alpha-blended by its antialiased rim,
// showing the scope's picture through the eyepiece's optics and reticle.
// Uses the scene + scope root signature: the frame set (the main view's) and
// the pass set, a transient table of the lens uniforms (in the upload ring,
// per frame) and the picture, with the linear clamp sampler.
//
// Borrows everything it is given. Neither copyable nor movable. Not
// thread-safe.
class ScopeLensPass {
 public:
  ScopeLensPass(const D3d12Context& context, ShaderPrograms& programs,
                const RootSignature& root_signature,
                const FrameGlobals& frame_globals, const Samplers& samplers,
                UploadRing& upload_ring);

  ScopeLensPass(const ScopeLensPass&) = delete;
  ScopeLensPass& operator=(const ScopeLensPass&) = delete;

  // The state the picture must be in when Draw() samples it: the pass
  // table holds the vertex stage's uniforms too, so the table is visible to
  // every stage.
  static constexpr D3D12_RESOURCE_STATES kPictureState =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

  // Makes Draw() show `picture` (in kPictureState when drawn; it must
  // outlive this object). Draw() records nothing before the first call.
  void BindPicture(const Image& picture) { picture_ = &picture; }

  // Records the eyepiece, if the view has a scope. Call inside the main
  // scene pass after the opaque meshes, so their depth hides the glass; the
  // picture must be in kPictureState. Sets its own root signature.
  void Draw(const FrameContext& ctx) const;

 private:
  const D3d12Context& context_;
  const RootSignature& root_signature_;
  SceneRootParams params_;
  const FrameGlobals& frame_globals_;
  const Samplers& samplers_;
  UploadRing& upload_ring_;
  // Where the uniforms and the picture go in the pass table.
  uint32_t uniforms_offset_ = 0;
  uint32_t picture_offset_ = 0;
  uint32_t table_size_ = 0;
  const Image* picture_ = nullptr;
  // Replaced on a shader reload.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_SCOPE_LENS_PASS_H_
