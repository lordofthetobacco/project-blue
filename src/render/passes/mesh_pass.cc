#include "render/passes/mesh_pass.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "core/path.h"
#include "render/draw_list.h"
#include "render/render_targets.h"

namespace blue::render {

namespace {

constexpr auto kVertexBindings = VertexBindings();
constexpr auto kVertexAttributes = VertexAttributes();

}  // namespace

MeshPass::MeshPass(const VulkanContext& context, ShaderPrograms& programs,
                   const std::string& shader_dir,
                   VkDescriptorSetLayout frame_layout,
                   const MaterialSystem& materials,
                   const std::vector<Mesh>& meshes)
    : device_(context.device()),
      frame_layout_(frame_layout),
      materials_(materials),
      meshes_(meshes) {
  programs.Add(
      core::JoinPath(shader_dir, "mesh.vert"),
      core::JoinPath(shader_dir, "mesh.frag"),
      [this](std::span<const uint32_t> vertex,
             std::span<const uint32_t> fragment) {
        const std::array<VkDescriptorSetLayout, 2> set_layouts{
            frame_layout_, materials_.set_layout()};
        for (size_t i = 0; i < kMaterialVariantCount; ++i) {
          const auto variant = static_cast<MaterialVariant>(i);
          // constant_id 0: alpha mask, 1: double sided (shaders/mesh.frag).
          const std::array<uint32_t, 2> constants{
              IsMasked(variant) ? 1u : 0u, IsDoubleSided(variant) ? 1u : 0u};
          pipelines_[i] = std::make_unique<Pipeline>(
              device_,
              GraphicsPipelineDesc{
                  .vertex_spirv = vertex,
                  .fragment_spirv = fragment,
                  .fragment_constants = constants,
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
                  // vertex shaders compute an invariant gl_Position with the
                  // same expression, so the depths match exactly.
                  .depth_compare_op = VK_COMPARE_OP_EQUAL,
                  .depth_write = false,
              });
        }
      });
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

}  // namespace blue::render
