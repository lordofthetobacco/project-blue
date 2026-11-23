#include "render/d3d12/passes/mesh_pass.h"

#include <d3d12.h>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include "render/binding_layout.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/draw_record.h"
#include "render/d3d12/pipeline.h"
#include "render/d3d12/render_targets.h"
#include "render/draw_list.h"
#include "render/shader_manifest.h"

namespace blue::render::d3d12 {

MeshPass::MeshPass(const D3d12Context& context, ShaderPrograms& programs,
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
  // One program per variant: alpha mask and double sided are compile-time
  // switches of shaders/mesh.slang.
  constexpr std::array<ShaderProgram, kMaterialVariantCount> kPrograms = {
      ShaderProgram::kMesh,        // kOpaque
      ShaderProgram::kMeshMasked,  // kMasked
      ShaderProgram::kMeshDouble,  // kDoubleSided
      ShaderProgram::kMeshMaskedDouble,
  };
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    programs.Add(kPrograms[i], [this, i](std::span<const std::byte> vertex,
                                         std::span<const std::byte> pixel) {
      const auto variant = static_cast<MaterialVariant>(i);
      GraphicsPipelineDesc desc;
      desc.root_signature = root_signature_.get();
      desc.vertex = vertex;
      desc.pixel = pixel;
      desc.input_layout = VertexInputLayout();
      desc.rtv_formats[0] = RenderTargets::kHdrFormat;
      desc.rtv_count = 1;
      desc.dsv_format = RenderTargets::kDepthFormat;
      desc.cull =
          IsDoubleSided(variant) ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
      desc.depth_test = true;
      desc.depth_write = false;
      desc.depth_func = D3D12_COMPARISON_FUNC_EQUAL;
      desc.name = L"blue mesh pipeline";
      pipelines_[i] = CreateGraphicsPipeline(context_.device(), desc);
    });
  }
}

void MeshPass::Draw(const FrameContext& ctx) const {
  ctx.cmd->SetGraphicsRootSignature(root_signature_.get());
  frame_globals_.Bind(ctx.cmd, params_, ctx.frame);
  DrawListPipelines pipelines;
  pipelines.material_param = params_.pass_table;
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    pipelines.pipelines[i] = pipelines_[i].Get();
    pipelines.bind_material[i] = true;
  }
  // Every material samples its textures with the material sampler.
  ctx.cmd->SetGraphicsRootDescriptorTable(params_.pass_sampler,
                                          materials_.sampler());
  RecordDrawList(ctx.cmd, ctx.draw_list, pipelines, ctx.view, materials_,
                 meshes_, &ctx.stats);
}

}  // namespace blue::render::d3d12
