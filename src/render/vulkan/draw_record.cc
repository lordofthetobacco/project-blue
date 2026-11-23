#include "render/vulkan/draw_record.h"

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>

namespace blue::render::vulkan {

namespace {

// Must match DrawConstants in shaders/mesh.vert and shaders/depth.vert.
struct DrawConstants {
  glm::mat4 model;
};
static_assert(sizeof(DrawConstants) == kDrawConstantsRange.size);

}  // namespace

DrawListResources DrawListSource::resources() const {
  return {
      .context = this,
      .mesh_index_count =
          [](const void* context, MeshId mesh, uint32_t& index_count) {
            const auto& source = *static_cast<const DrawListSource*>(context);
            const auto index = static_cast<size_t>(mesh);
            if (index >= source.meshes_.size() ||
                !source.meshes_[index].resident()) {
              return false;
            }
            index_count = source.meshes_[index].index_count();
            return true;
          },
      .resolve_material =
          [](const void* context, MaterialId material,
             MaterialVariant& variant) {
            const auto& source = *static_cast<const DrawListSource*>(context);
            const MaterialId resolved = source.materials_.Resolve(material);
            variant = source.materials_.variant(resolved);
            return resolved;
          },
  };
}

void RecordDrawList(VkCommandBuffer cmd, std::span<const DrawEntry> entries,
                    const DrawListPipelines& pipelines,
                    VkDescriptorSet frame_set, const SceneView& view,
                    const MaterialSystem& materials,
                    const std::vector<Mesh>& meshes, RenderStats* stats) {
  const Pipeline* bound_pipeline = nullptr;
  VkDescriptorSet bound_material = VK_NULL_HANDLE;
  const Mesh* bound_mesh = nullptr;
  uint32_t pushed_item = UINT32_MAX;
  for (const DrawEntry& entry : entries) {
    const auto variant = static_cast<size_t>(entry.variant);
    const Pipeline* pipeline = pipelines.pipelines[variant];
    if (pipeline == nullptr) {
      continue;
    }
    if (pipeline != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        pipeline->handle());
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline->layout(), 0, 1, &frame_set, 0, nullptr);
      bound_pipeline = pipeline;
      bound_material = VK_NULL_HANDLE;
      pushed_item = UINT32_MAX;
    }
    if (pipelines.bind_material[variant]) {
      const VkDescriptorSet material_set =
          materials.descriptor_set(entry.material);
      if (material_set != bound_material) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipeline->layout(), 1, 1, &material_set, 0,
                                nullptr);
        bound_material = material_set;
      }
    }
    const DrawItem& item = view.draws[entry.item];
    const Mesh& mesh = meshes[static_cast<size_t>(item.mesh)];
    if (&mesh != bound_mesh) {
      mesh.Bind(cmd);
      bound_mesh = &mesh;
    }
    if (entry.item != pushed_item) {
      const DrawConstants constants{.model = item.transform};
      vkCmdPushConstants(cmd, pipeline->layout(),
                         kDrawConstantsRange.stageFlags, 0, sizeof(constants),
                         &constants);
      pushed_item = entry.item;
    }
    Mesh::DrawRange(cmd, entry.first_index, entry.index_count);
    if (stats != nullptr) {
      ++stats->draw_calls;
      stats->triangles += entry.index_count / 3;
    }
  }
}

}  // namespace blue::render::vulkan
