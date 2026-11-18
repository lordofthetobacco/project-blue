#include "render/draw_list.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>

namespace blue::render {

namespace {

// Must match DrawConstants in shaders/mesh.vert and shaders/depth.vert.
struct DrawConstants {
  glm::mat4 model;
};
static_assert(sizeof(DrawConstants) == kDrawConstantsRange.size);

// Sort key: variant in the top 2 bits, then 30 bits of material, then the
// mesh, so draws group by pipeline, then material, then vertex buffers.
constexpr int kVariantShift = 62;
constexpr int kMaterialShift = 32;
constexpr uint64_t kMaterialMask = (uint64_t{1} << 30) - 1;

uint64_t SortKey(MaterialVariant variant, MaterialId material, MeshId mesh) {
  return (static_cast<uint64_t>(variant) << kVariantShift) |
         ((static_cast<uint64_t>(material) & kMaterialMask) << kMaterialShift) |
         static_cast<uint64_t>(mesh);
}

}  // namespace

void DrawList::Build(const SceneView& view,
                     std::span<const camera::Aabb> world_bounds,
                     const camera::Frustum& frustum,
                     const MaterialSystem& materials,
                     const std::vector<Mesh>& meshes, RenderStats& stats) {
  entries_.clear();
  for (size_t i = 0; i < view.draws.size(); ++i) {
    if (!frustum.Intersects(world_bounds[i])) {
      ++stats.culled_draws;
      continue;
    }
    const DrawItem& item = view.draws[i];
    const auto add = [&](uint32_t first_index, uint32_t index_count,
                         MaterialId material) {
      if (item.material_override) {
        material = *item.material_override;
      }
      const MaterialVariant variant = materials.variant(material);
      entries_.push_back({
          .key = SortKey(variant, material, item.mesh),
          .item = static_cast<uint32_t>(i),
          .first_index = first_index,
          .index_count = index_count,
          .material = material,
          .variant = variant,
      });
    };
    if (item.submeshes.empty()) {
      add(0, meshes[static_cast<size_t>(item.mesh)].index_count(),
          kDefaultMaterial);
    } else {
      for (const SubmeshDraw& submesh : item.submeshes) {
        add(submesh.first_index, submesh.index_count, submesh.material);
      }
    }
  }
  std::ranges::sort(entries_, [](const DrawEntry& a, const DrawEntry& b) {
    return a.key != b.key ? a.key < b.key : a.item < b.item;
  });
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

}  // namespace blue::render
