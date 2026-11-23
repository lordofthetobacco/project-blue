#include "render/draw_list.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>

#include "camera/frustum.h"
#include "material/material.h"
#include "render/scene_view.h"

namespace blue::render {

namespace {

// Meshes and materials as a backend would describe them: mesh i has
// index_counts[i] indices (0: released); material i has variants[i], and an
// id past the end resolves to kDefaultMaterial.
struct FakeResources {
  std::vector<uint32_t> index_counts;
  std::vector<MaterialVariant> variants;

  [[nodiscard]] DrawListResources resources() const {
    return {
        .context = this,
        .mesh_index_count =
            [](const void* context, MeshId mesh, uint32_t& index_count) {
              const auto& self = *static_cast<const FakeResources*>(context);
              const auto index = static_cast<size_t>(mesh);
              if (index >= self.index_counts.size() ||
                  self.index_counts[index] == 0) {
                return false;
              }
              index_count = self.index_counts[index];
              return true;
            },
        .resolve_material =
            [](const void* context, MaterialId material,
               MaterialVariant& variant) {
              const auto& self = *static_cast<const FakeResources*>(context);
              auto index = static_cast<size_t>(material);
              if (index >= self.variants.size()) {
                index = static_cast<size_t>(kDefaultMaterial);
              }
              variant = self.variants[index];
              return static_cast<MaterialId>(index);
            },
    };
  }
};

// A unit box around the origin, as every item's bounds.
constexpr camera::Aabb kUnitBox{.min = glm::vec3(-0.5f),
                                .max = glm::vec3(0.5f)};

// A frustum that sees the origin from z = 5.
camera::Frustum OriginFrustum() {
  const glm::mat4 projection =
      glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
  const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f, 0.0f, 5.0f),
                                       glm::vec3(0.0f), glm::vec3(0, 1, 0));
  return camera::Frustum(projection * view);
}

}  // namespace

TEST_CASE("VariantOf follows alpha mode and sidedness") {
  material::MaterialDesc desc;
  CHECK(VariantOf(desc) == MaterialVariant::kOpaque);
  desc.double_sided = true;
  CHECK(VariantOf(desc) == MaterialVariant::kDoubleSided);
  desc.alpha_mode = material::AlphaMode::kMask;
  CHECK(VariantOf(desc) == MaterialVariant::kMaskedDoubleSided);
  desc.double_sided = false;
  CHECK(VariantOf(desc) == MaterialVariant::kMasked);
  CHECK(IsMasked(MaterialVariant::kMasked));
  CHECK_FALSE(IsDoubleSided(MaterialVariant::kMasked));
  CHECK(IsDoubleSided(MaterialVariant::kMaskedDoubleSided));
}

TEST_CASE("DrawList culls, skips released meshes and sorts by variant") {
  const FakeResources fake{
      .index_counts = {36, 0, 12},
      .variants = {MaterialVariant::kOpaque, MaterialVariant::kMasked,
                   MaterialVariant::kDoubleSided},
  };
  std::array<DrawItem, 4> draws{};
  // 0: mesh 0 with two submeshes, the first masked, the second opaque.
  const std::array<SubmeshDraw, 2> submeshes{{
      {.first_index = 0, .index_count = 18, .material = MaterialId{1}},
      {.first_index = 18, .index_count = 18, .material = MaterialId{0}},
  }};
  draws[0].mesh = MeshId{0};
  draws[0].submeshes = submeshes;
  // 1: released mesh 1, skipped without counting as culled.
  draws[1].mesh = MeshId{1};
  // 2: mesh 2, whole mesh, unknown material override -> default.
  draws[2].mesh = MeshId{2};
  draws[2].material_override = MaterialId{99};
  // 3: far behind the camera: culled.
  draws[3].mesh = MeshId{2};
  draws[3].transform = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 50));
  SceneView view;
  view.draws = draws;
  std::array<camera::Aabb, 4> bounds{kUnitBox, kUnitBox, kUnitBox, kUnitBox};
  bounds[3] = camera::TransformAabb(kUnitBox, draws[3].transform);

  RenderStats stats;
  DrawList list;
  list.Build(view, bounds, OriginFrustum(), fake.resources(), stats);

  CHECK(stats.culled_draws == 1);
  const std::span<const DrawEntry> entries = list.entries();
  REQUIRE(entries.size() == 3);
  // Opaque first (item 0's second submesh, then item 2), masked last.
  CHECK(entries[0].item == 0);
  CHECK(entries[0].first_index == 18);
  CHECK(entries[0].variant == MaterialVariant::kOpaque);
  CHECK(entries[1].item == 2);
  CHECK(entries[1].index_count == 12);
  CHECK(entries[1].material == kDefaultMaterial);
  CHECK(entries[2].item == 0);
  CHECK(entries[2].variant == MaterialVariant::kMasked);
  for (size_t i = 1; i < entries.size(); ++i) {
    CHECK(entries[i - 1].key <= entries[i].key);
  }
}

TEST_CASE("A scope's draw list leaves out the items hidden in the scope") {
  const FakeResources fake{.index_counts = {3},
                           .variants = {MaterialVariant::kOpaque}};
  std::array<DrawItem, 2> draws{};
  draws[1].hidden_in_scope = true;
  SceneView view;
  view.draws = draws;
  const std::array<camera::Aabb, 2> bounds{kUnitBox, kUnitBox};
  RenderStats stats;
  DrawList list;
  list.Build(view, bounds, OriginFrustum(), fake.resources(), stats,
             /*scope_view=*/true);
  REQUIRE(list.entries().size() == 1);
  CHECK(list.entries()[0].item == 0);
  CHECK(stats.culled_draws == 0);

  list.Build(view, bounds, OriginFrustum(), fake.resources(), stats);
  CHECK(list.entries().size() == 2);
}

}  // namespace blue::render
