#include "render/draw_list.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <glm/mat4x4.hpp>

#include "shaders/bindings.h"

namespace blue::render {

namespace {

// Must match DrawConstants in shaders/mesh.vert.metal and
// shaders/depth.vert.metal.
struct DrawConstants {
  glm::mat4 model;
};
static_assert(sizeof(DrawConstants) == 64);

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
                     const std::vector<Mesh>& meshes, RenderStats& stats,
                     bool scope_view) {
  entries_.clear();
  for (size_t i = 0; i < view.draws.size(); ++i) {
    if (scope_view && view.draws[i].hidden_in_scope) {
      continue;
    }
    if (!frustum.Intersects(world_bounds[i])) {
      ++stats.culled_draws;
      continue;
    }
    const DrawItem& item = view.draws[i];
    const auto mesh_index = static_cast<size_t>(item.mesh);
    if (mesh_index >= meshes.size() || !meshes[mesh_index].resident()) {
      continue;  // Released or unknown mesh: nothing to draw.
    }
    const auto add = [&](uint32_t first_index, uint32_t index_count,
                         MaterialId material) {
      if (item.material_override) {
        material = *item.material_override;
      }
      material = materials.Resolve(material);
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
      add(0, meshes[mesh_index].index_count(), kDefaultMaterial);
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

void RecordDrawList(id<MTLRenderCommandEncoder> encoder,
                    std::span<const DrawEntry> entries,
                    const DrawListPipelines& pipelines, const SceneView& view,
                    const MaterialSystem& materials,
                    const std::vector<Mesh>& meshes, RenderStats* stats) {
  const Pipeline* bound_pipeline = nullptr;
  std::optional<MaterialId> bound_material;
  const Mesh* bound_mesh = nullptr;
  uint32_t set_item = UINT32_MAX;
  for (const DrawEntry& entry : entries) {
    const auto variant = static_cast<size_t>(entry.variant);
    const Pipeline* pipeline = pipelines.pipelines[variant];
    if (pipeline == nullptr) {
      continue;
    }
    // Encoder bindings survive pipeline changes, so the material and the
    // model matrix stay set across them.
    if (pipeline != bound_pipeline) {
      pipeline->Bind(encoder);
      bound_pipeline = pipeline;
    }
    if (pipelines.bind_material[variant] && bound_material != entry.material) {
      materials.Bind(encoder, entry.material);
      bound_material = entry.material;
    }
    const DrawItem& item = view.draws[entry.item];
    const Mesh& mesh = meshes[static_cast<size_t>(item.mesh)];
    if (&mesh != bound_mesh) {
      mesh.Bind(encoder);
      bound_mesh = &mesh;
    }
    if (entry.item != set_item) {
      const DrawConstants constants{.model = item.transform};
      [encoder setVertexBytes:&constants
                       length:sizeof(constants)
                      atIndex:kBufferDraw];
      set_item = entry.item;
    }
    mesh.DrawRange(encoder, entry.first_index, entry.index_count);
    if (stats != nullptr) {
      ++stats->draw_calls;
      stats->triangles += entry.index_count / 3;
    }
  }
}

}  // namespace blue::render
