#ifndef BLUE_RENDER_D3D12_PASSES_SHADOW_PASS_H_
#define BLUE_RENDER_D3D12_PASSES_SHADOW_PASS_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "render/d3d12/image.h"
#include "render/d3d12/mesh.h"
#include "render/d3d12/passes/frame_context.h"
#include "render/d3d12/root_signature.h"
#include "render/d3d12/shader_programs.h"

namespace blue::render::d3d12 {

class D3d12Context;

// Renders the sun's cascaded shadow map (shaders/shadow.slang): one
// depth-only layer of a D16 2D array per cascade, every draw item whose
// world bounds reach the cascade, positions only (alpha-masked materials
// cast solid shadows), no culling, standard depth (LESS, clear 1), depth
// clamp and the Vulkan backend's slope-scaled depth bias. One map for all
// frames in flight: the single queue orders this pass's writes after the
// previous frame's reads. Uses the scene + material root signature (root
// constants only). Borrows everything it is given. Neither copyable nor
// movable.
class ShadowPass {
 public:
  static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_D16_UNORM;

  ShadowPass(const D3d12Context& context, ShaderPrograms& programs,
             const RootSignature& root_signature,
             const std::vector<Mesh>& meshes);

  ShadowPass(const ShadowPass&) = delete;
  ShadowPass& operator=(const ShadowPass&) = delete;

  // Renders the cascades of ctx (ctx.plan.shadows). Leaves the map in
  // PIXEL_SHADER_RESOURCE, where it also is between frames (initialized lit).
  void Record(const FrameContext& ctx);

  // The cascades (a Texture2DArray SRV of R16_UNORM), FrameGlobals t1.
  [[nodiscard]] const Image& map() const { return map_; }

 private:
  const D3d12Context& context_;
  const RootSignature& root_signature_;
  const std::vector<Mesh>& meshes_;
  Image map_;
  // Replaced on a shader reload.
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_PASSES_SHADOW_PASS_H_
