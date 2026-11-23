#ifndef BLUE_RENDER_D3D12_PASSES_SKYBOX_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_SKYBOX_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include "render/d3d12/frame_globals.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Environment;
class Samplers;

// Draws the environment's sky cube behind the scene (shaders/skybox.slang):
// a full-screen triangle at reversed-Z depth 0 that only covers pixels the
// opaque meshes left at the cleared depth (GREATER_EQUAL, no depth write).
// Uses the scene + sky root signature. Borrows everything it is given.
// Neither copyable nor movable.
class SkyboxPass {
 public:
  SkyboxPass(const D3d12Context& context, ShaderPrograms& programs,
             const RootSignature& root_signature,
             const FrameGlobals& frame_globals, const Environment& environment,
             const Samplers& samplers);

  SkyboxPass(const SkyboxPass&) = delete;
  SkyboxPass& operator=(const SkyboxPass&) = delete;

  // Records the sky. Call inside the scene pass after the opaque meshes;
  // sets its own root signature.
  void Draw(const FrameContext& ctx) const;

 private:
  const D3d12Context& context_;
  const RootSignature& root_signature_;
  SceneRootParams params_;
  const FrameGlobals& frame_globals_;
  const Environment& environment_;
  const Samplers& samplers_;
  // Replaced on a shader reload.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_SKYBOX_PASS_H_
