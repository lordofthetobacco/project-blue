#ifndef BLUE_RENDER_D3D12_PASSES_RETICLE_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_RETICLE_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include "render/d3d12/frame_globals.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;

// Draws the reflex sight of SceneView::sight (shaders/reticle.slang): an
// additive, depth-tested quad without a vertex buffer, with a scene root
// signature (it reads only the frame set and its root constants). Borrows
// everything it is given. Neither copyable nor movable. Not thread-safe.
class ReticlePass {
 public:
  ReticlePass(const D3d12Context& context, ShaderPrograms& programs,
              const RootSignature& root_signature,
              const FrameGlobals& frame_globals);

  ReticlePass(const ReticlePass&) = delete;
  ReticlePass& operator=(const ReticlePass&) = delete;

  // Records the sight, if the view has one. Call inside the scene pass after
  // the opaque meshes, so their depth hides the window; sets its own root
  // signature.
  void Draw(const FrameContext& ctx) const;

 private:
  const D3d12Context& context_;
  const RootSignature& root_signature_;
  SceneRootParams params_;
  const FrameGlobals& frame_globals_;
  // Replaced on a shader reload.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_RETICLE_PASS_H_
