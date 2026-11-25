#include "render/d3d12/passes/shadow_pass.h"

#include <d3d12.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include "camera/frustum.h"
#include "lighting/cascades.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/draw_record.h"
#include "render/d3d12/image.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/root_layout.h"
#include "render/draw_list.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// The Vulkan backend's depth bias: constant 1.25 and slope 1.75 (in units
// of the D16 resolution r). Direct3D's constant bias is an integer count of
// r; 1 is the nearest to 1.25 (2 would push shadows off thin casters).
constexpr INT kDepthBias = 1;
constexpr float kSlopeScaledDepthBias = 1.75f;
// The cascades use standard depth: the far plane is 1.
constexpr float kDepthClearValue = 1.0f;
// The shaders sample the map with the Vulkan backend's layout, whose shadow
// viewport is not flipped: clip y = -1 is texel row 0. Direct3D puts clip
// y = +1 there, so the cascades are rendered mirrored in y (no culling, so
// the winding does not matter) and both backends store the same texels.
const glm::mat4 kVulkanRows =
    glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f));

}  // namespace

ShadowPass::ShadowPass(const D3d12Context& context, ShaderPrograms& programs,
                       const RootSignature& root_signature,
                       const std::vector<Mesh>& meshes)
    : context_(context), root_signature_(root_signature), meshes_(meshes) {
  constexpr uint32_t kSize = lighting::kShadowMapSize;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = kFormat;
  clear.DepthStencil = {.Depth = kDepthClearValue, .Stencil = 0};
  map_ = Image(context, {.extent = {kSize, kSize},
                         .layers = lighting::kCascadeCount,
                         .format = DXGI_FORMAT_R16_TYPELESS,
                         .flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                         .initial_state = D3D12_RESOURCE_STATE_DEPTH_WRITE,
                         .clear_value = clear,
                         .name = L"blue shadow map"});
  for (uint32_t layer = 0; layer < lighting::kCascadeCount; ++layer) {
    map_.AddLayerDsv(kFormat, layer);
  }
  map_.CreateSrv(DXGI_FORMAT_R16_UNORM, /*shader_visible=*/false);
  // Lit until the first frame with shadows (and initialized, as placed depth
  // targets must be).
  context.SubmitImmediate([this](ID3D12GraphicsCommandList* cmd) {
    for (uint32_t layer = 0; layer < lighting::kCascadeCount; ++layer) {
      cmd->ClearDepthStencilView(map_.layer_dsv(layer), D3D12_CLEAR_FLAG_DEPTH,
                                 kDepthClearValue, 0, 0, nullptr);
    }
    BarrierBatch barriers(cmd);
    map_.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  });

  programs.Add(ShaderProgram::kShadow, [this](std::span<const std::byte> vertex,
                                              std::span<const std::byte>) {
    GraphicsPipelineDesc desc;
    desc.root_signature = root_signature_.get();
    desc.vertex = vertex;
    desc.input_layout = VertexInputLayout();
    desc.dsv_format = kFormat;
    // Open meshes (ground) must cast from both sides.
    desc.cull = D3D12_CULL_MODE_NONE;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_func = D3D12_COMPARISON_FUNC_LESS;
    desc.depth_clip = false;  // Depth clamp: casters behind the near plane.
    desc.depth_bias = kDepthBias;
    desc.slope_scaled_depth_bias = kSlopeScaledDepthBias;
    desc.name = L"blue shadow pipeline";
    pipeline_ = CreateGraphicsPipeline(context_.device(), desc);
  });
}

void ShadowPass::Record(const FrameContext& ctx) {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  {
    BarrierBatch barriers(cmd);
    map_.Transition(barriers, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  constexpr uint32_t kSize = lighting::kShadowMapSize;
  SetViewport(cmd, {kSize, kSize});
  cmd->SetGraphicsRootSignature(root_signature_.get());
  cmd->SetPipelineState(pipeline_.Get());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  for (uint32_t layer = 0; layer < ctx.cascades.size(); ++layer) {
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = map_.layer_dsv(layer);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, kDepthClearValue, 0,
                               0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    // The cascade's box already extends towards the sun, so anything
    // outside it cannot shadow this slice.
    const lighting::Cascade& cascade = ctx.cascades[layer];
    const camera::Frustum cascade_frustum(cascade.view_projection);
    for (size_t i = 0; i < ctx.view.draws.size(); ++i) {
      if (!IsShadowCaster(ctx.view.draws[i])) {
        continue;
      }
      if (!cascade_frustum.Intersects(ctx.world_bounds[i])) {
        ++ctx.stats.shadow_culled_draws;
        continue;
      }
      const DrawItem& item = ctx.view.draws[i];
      const auto mesh_index = static_cast<size_t>(item.mesh);
      if (mesh_index >= meshes_.size() || !meshes_[mesh_index].resident()) {
        continue;
      }
      ++ctx.stats.shadow_draw_calls;
      const glm::mat4 light_model_view_proj =
          kVulkanRows * cascade.view_projection * item.transform;
      cmd->SetGraphicsRoot32BitConstants(
          kPushConstantsParam, kDrawConstantCount, &light_model_view_proj, 0);
      const Mesh& mesh = meshes_[mesh_index];
      mesh.Bind(cmd);
      Mesh::DrawRange(cmd, 0, mesh.index_count());
    }
  }
  BarrierBatch barriers(cmd);
  map_.Transition(barriers, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

}  // namespace blue::render::d3d12
