#include "render/d3d12/passes/skybox_pass.h"

#include <d3d12.h>

#include <cstddef>
#include <span>

#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/descriptor_heaps.h"
#include "render/d3d12/environment.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/d3d12/samplers.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// The vertex shader emits one triangle that covers the whole viewport.
constexpr UINT kVertexCount = 3;

}  // namespace

SkyboxPass::SkyboxPass(const D3d12Context& context, ShaderPrograms& programs,
                       const RootSignature& root_signature,
                       const FrameGlobals& frame_globals,
                       const Environment& environment, const Samplers& samplers)
    : context_(context),
      root_signature_(root_signature),
      params_(SceneRootParams::Of(root_signature.layout(), kSkyBindings)),
      frame_globals_(frame_globals),
      environment_(environment),
      samplers_(samplers) {
  programs.Add(ShaderProgram::kSkybox,
               [this](std::span<const std::byte> vertex,
                      std::span<const std::byte> pixel) {
                 GraphicsPipelineDesc desc;
                 desc.root_signature = root_signature_.get();
                 desc.vertex = vertex;
                 desc.pixel = pixel;
                 desc.rtv_formats[0] = RenderTargets::kHdrFormat;
                 desc.rtv_count = 1;
                 desc.dsv_format = RenderTargets::kDepthFormat;
                 // Only where nothing was drawn: the depth there is still the
                 // clear value 0, which the triangle at depth 0 equals.
                 desc.depth_test = true;
                 desc.depth_write = false;
                 desc.depth_func = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
                 desc.name = L"blue skybox pipeline";
                 pipeline_ = CreateGraphicsPipeline(context_.device(), desc);
               });
}

void SkyboxPass::Draw(const FrameContext& ctx) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  DescriptorHeaps& heaps = context_.descriptors();
  const Descriptor sky = heaps.AllocateTransient(1);
  heaps.Copy(1, sky.cpu, environment_.sky().staging_srv());
  cmd->SetGraphicsRootSignature(root_signature_.get());
  cmd->SetPipelineState(pipeline_.Get());
  frame_globals_.Bind(cmd, params_, ctx.frame);
  cmd->SetGraphicsRootDescriptorTable(params_.pass_table, sky.gpu);
  cmd->SetGraphicsRootDescriptorTable(params_.pass_sampler,
                                      samplers_.linear_clamp());
  cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  cmd->DrawInstanced(kVertexCount, 1, 0, 0);
  ++ctx.stats.draw_calls;
}

}  // namespace blue::render::d3d12
