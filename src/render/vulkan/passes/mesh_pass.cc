#include "render/vulkan/passes/mesh_pass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "render/binding_layout.h"
#include "render/shader_manifest.h"
#include "core/path.h"
#include "render/vulkan/draw_record.h"
#include "render/vulkan/render_targets.h"

namespace blue::render::vulkan {

namespace {

constexpr auto kVertexBindings = VertexBindings();
constexpr auto kVertexAttributes = VertexAttributes();

}  // namespace

MeshPass::MeshPass(const VulkanContext& context, ShaderPrograms& programs,
                   VkDescriptorSetLayout frame_layout,
                   const MaterialSystem& materials,
                   const std::vector<Mesh>& meshes)
    : device_(context.device()),
      frame_layout_(frame_layout),
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
    programs.Add(
        kPrograms[i], [this, i](std::span<const uint32_t> vertex,
                                std::span<const uint32_t> fragment) {
          const auto variant = static_cast<MaterialVariant>(i);
          const std::array<VkDescriptorSetLayout, 2> set_layouts{
              frame_layout_, materials_.set_layout()};
          pipelines_[i] = std::make_unique<Pipeline>(
              device_,
              GraphicsPipelineDesc{
                  .vertex_spirv = vertex,
                  .fragment_spirv = fragment,
                  .vertex_bindings = kVertexBindings,
                  .vertex_attributes = kVertexAttributes,
                  .set_layouts = set_layouts,
                  .push_constants = {&kDrawConstantsRange, 1},
                  .color_format = RenderTargets::kHdrFormat,
                  .depth_format = RenderTargets::kDepthFormat,
                  .cull_mode = IsDoubleSided(variant)
                                   ? VkCullModeFlags{VK_CULL_MODE_NONE}
                                   : VkCullModeFlags{VK_CULL_MODE_BACK_BIT},
                  // Only the surface the depth pre-pass kept passes; both
                  // vertex shaders compute an invariant position with the
                  // same expression (ClipPosition() in shaders/common.slang),
                  // so the depths match exactly.
                  .depth_compare_op = VK_COMPARE_OP_EQUAL,
                  .depth_write = false,
              });
        });
  }
}

void MeshPass::Draw(const FrameContext& ctx) const {
  DrawListPipelines pipelines;
  for (size_t i = 0; i < kMaterialVariantCount; ++i) {
    pipelines.pipelines[i] = pipelines_[i].get();
    pipelines.bind_material[i] = true;
  }
  RecordDrawList(ctx.cmd, ctx.draw_list, pipelines, ctx.frame_set, ctx.view,
                 materials_, meshes_, &ctx.stats);
}

}  // namespace blue::render::vulkan
