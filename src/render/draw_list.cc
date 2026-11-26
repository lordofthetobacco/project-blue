#include "render/draw_list.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace blue::render {

namespace {

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

MaterialVariant VariantOf(const material::MaterialDesc& desc) {
  const bool masked = desc.alpha_mode == material::AlphaMode::kMask;
  if (desc.double_sided) {
    return masked ? MaterialVariant::kMaskedDoubleSided
                  : MaterialVariant::kDoubleSided;
  }
  return masked ? MaterialVariant::kMasked : MaterialVariant::kOpaque;
}

void DrawList::Build(const SceneView& view,
                     std::span<const camera::Aabb> world_bounds,
                     const camera::Frustum& frustum,
                     const DrawListResources& resources, RenderStats& stats,
                     bool scope_view) {
  entries_.clear();
  for (size_t i = 0; i < view.draws.size(); ++i) {
    // The camera passes never see a shadow-only item; the shadow passes
    // read view.draws directly.
    if (view.draws[i].shadow_only ||
        (scope_view && view.draws[i].hidden_in_scope)) {
      continue;
    }
    if (!frustum.Intersects(world_bounds[i])) {
      ++stats.culled_draws;
      continue;
    }
    const DrawItem& item = view.draws[i];
    uint32_t mesh_index_count = 0;
    if (!resources.mesh_index_count(resources.context, item.mesh,
                                    mesh_index_count)) {
      continue;  // Released or unknown mesh: nothing to draw.
    }
    const auto add = [&](uint32_t first_index, uint32_t index_count,
                         MaterialId material) {
      if (item.material_override) {
        material = *item.material_override;
      }
      MaterialVariant variant = MaterialVariant::kOpaque;
      material =
          resources.resolve_material(resources.context, material, variant);
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
      add(0, mesh_index_count, kDefaultMaterial);
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

}  // namespace blue::render
