#include "render/d3d12/passes/depth_prepass.h"

#include <d3d12.h>

#include <cstddef>
#include <span>
#include <vector>

#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/draw_record.h"
#include "render/d3d12/image.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/draw_list.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

namespace {

// Reversed-Z: infinity is 0.
constexpr float kDepthClearValue = 0.0f;

}  // namespace

DepthPrepass::DepthPrepass(const D3d12Context& context,
                           ShaderPrograms& programs,
                           const RootSignature& root_signature,
                           const FrameGlobals& frame_globals,
                           const MaterialSystem& materials,
                           const std::vector<Mesh>& meshes)
    : context_(context),
      root_signature_(root_signature),
      params_(SceneRootParams::Of(root_signature.layout(), kMaterialBindings)),
      frame_globals_(frame_globals),
      materials_(materials),
      meshes_(meshes) {
  programs.Add(ShaderProgram::kDepth, [this](std::span<const std::byte> vertex,
                                             std::span<const std::byte> pixel) {
    CreatePipelines(/*masked=*/false, vertex, pixel);
  });
  programs.Add(ShaderProgram::kDepthMasked,
               [this](std::span<const std::byte> vertex,
                      std::span<const std::byte> pixel) {
                 CreatePipelines(/*masked=*/true, vertex, pixel);
               });
}

void DepthPrepass::CreatePipelines(bool masked,
                                   std::span<const std::byte> vertex,
                                   std::span<const std::byte> pixel) {
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    const auto variant = static_cast<MaterialVariant>(i);
    if (IsMasked(variant) != masked) {
      continue;
    }
    GraphicsPipelineDesc desc;
    desc.root_signature = root_signature_.get();
    desc.vertex = vertex;
    desc.pixel = pixel;
    desc.input_layout = VertexInputLayout();
    desc.dsv_format = RenderTargets::kDepthFormat;
    desc.cull = IsDoubleSided(variant) ? D3D12_CULL_MODE_NONE
                                       : D3D12_CULL_MODE_BACK;
    desc.depth_test = true;
    desc.depth_write = true;
    desc.depth_func = D3D12_COMPARISON_FUNC_GREATER;
    desc.name = L"blue depth pre-pass pipeline";
    pipelines_[i] = CreateGraphicsPipeline(context_.device(), desc);
  }
}

void DepthPrepass::Record(const FrameContext& ctx) const {
  ID3D12GraphicsCommandList* cmd = ctx.cmd;
  Image& depth = ctx.targets.depth();
  {
    BarrierBatch barriers(cmd);
    depth.Transition(barriers, D3D12_RESOURCE_STATE_DEPTH_WRITE);
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = depth.dsv();
  cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, kDepthClearValue, 0,
                             0, nullptr);
  cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
  SetViewport(cmd, ctx.targets.extent());
  cmd->SetGraphicsRootSignature(root_signature_.get());
  frame_globals_.Bind(cmd, params_, ctx.frame);

  DrawListPipelines pipelines;
  pipelines.material_param = params_.pass_table;
  // Masked materials sample their base colour.
  cmd->SetGraphicsRootDescriptorTable(params_.pass_sampler,
                                      materials_.sampler());
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    pipelines.pipelines[i] = pipelines_[i].Get();
    pipelines.bind_material[i] = IsMasked(static_cast<MaterialVariant>(i));
  }
  RecordDrawList(cmd, ctx.draw_list, pipelines, ctx.view, materials_, meshes_,
                 /*stats=*/nullptr);

  BarrierBatch barriers(cmd);
  depth.Transition(barriers, RenderTargets::kDepthReadState);
}

}  // namespace blue::render::d3d12
