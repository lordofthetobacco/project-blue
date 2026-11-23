#include "render/passes/mesh_pass.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "core/path.h"
#include "render/draw_list.h"
#include "render/render_targets.h"
#include "shaders/bindings.h"

namespace blue::render {

MeshPass::MeshPass(const MetalContext& context, ShaderPrograms& programs,
                   const std::string& shader_dir,
                   const MaterialSystem& materials,
                   const std::vector<Mesh>& meshes)
    : materials_(materials), meshes_(meshes) {
  programs.Add(
      core::JoinPath(shader_dir, "mesh.vert.metal"),
      core::JoinPath(shader_dir, "mesh.frag.metal"),
      [this, &context](const shader::ShaderLibrary& vertex,
                       const shader::ShaderLibrary* fragment)
          -> std::optional<std::string> {
        // All of them are created before any is replaced, so a failure
        // leaves the previous set intact.
        std::array<std::unique_ptr<Pipeline>, kMaterialVariantCount> created;
        for (size_t i = 0; i < kMaterialVariantCount; ++i) {
          const auto variant = static_cast<MaterialVariant>(i);
          std::array<bool, 2> constants{};
          constants[kConstantAlphaMask] = IsMasked(variant);
          constants[kConstantDoubleSided] = IsDoubleSided(variant);
          const GraphicsPipelineDesc desc{
              .vertex = &vertex,
              .fragment = fragment,
              .fragment_constants = constants,
              .vertex_descriptor = MeshVertexDescriptor(),
              .color_format = RenderTargets::kHdrFormat,
              .depth_format = RenderTargets::kDepthFormat,
              .cull_mode =
                  IsDoubleSided(variant) ? MTLCullModeNone : MTLCullModeBack,
              // Only the surface the depth pre-pass kept passes; both vertex
              // shaders compute an invariant position with the same
              // expression, so the depths match exactly.
              .depth_compare = MTLCompareFunctionEqual,
              .depth_write = false,
              .label = "mesh",
          };
          auto pipeline = Pipeline::Create(context, desc);
          if (!pipeline.ok()) {
            return pipeline.error();
          }
          created[i] = std::move(pipeline).value();
        }
        pipelines_ = std::move(created);
        return std::nullopt;
      });
}

void MeshPass::Draw(const FrameContext& ctx,
                    id<MTL4RenderCommandEncoder> encoder) const {
  DrawListPipelines pipelines;
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    pipelines.pipelines[i] = pipelines_[i].get();
    pipelines.bind_material[i] = true;
  }
  RecordDrawList(ctx.rec, encoder, ctx.draw_list, pipelines, ctx.view,
                 materials_, meshes_, &ctx.stats);
}

}  // namespace blue::render
