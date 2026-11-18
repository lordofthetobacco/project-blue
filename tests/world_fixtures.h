#ifndef BLUE_TESTS_WORLD_FIXTURES_H_
#define BLUE_TESTS_WORLD_FIXTURES_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "asset/mesh_data.h"
#include "material/material.h"
#include "prefab/prefab.h"
#include "render/scene_view.h"
#include "render/texture_cache.h"
#include "world/gpu_backend.h"

namespace blue::testing {

// A GpuBackend that hands out increasing ids and records every call.
class FakeGpu final : public world::GpuBackend {
 public:
  render::MeshId UploadMesh(const prefab::Mesh& /*mesh*/) override {
    ++mesh_uploads;
    return static_cast<render::MeshId>(next_mesh++);
  }
  std::vector<render::TextureId> UploadTextures(
      std::span<const prefab::Texture> textures) override {
    std::vector<render::TextureId> ids;
    for (size_t i = 0; i < textures.size(); ++i) {
      ids.push_back(static_cast<render::TextureId>(next_texture++));
    }
    texture_uploads += static_cast<int>(textures.size());
    return ids;
  }
  render::MaterialId CreateMaterial(
      const material::MaterialDesc& desc,
      const render::MaterialTextures& textures) override {
    descs.push_back(desc);
    texture_sets.push_back(textures);
    return static_cast<render::MaterialId>(next_material++);
  }
  void UpdateMaterial(render::MaterialId id,
                      const material::MaterialDesc& desc) override {
    updates.emplace_back(id, desc);
  }
  void ReleaseMesh(render::MeshId id) override {
    released_meshes.push_back(id);
  }
  void ReleaseTextures(std::span<const render::TextureId> ids) override {
    released_textures.insert(released_textures.end(), ids.begin(), ids.end());
  }
  void ReleaseMaterial(render::MaterialId id) override {
    released_materials.push_back(id);
  }

  // Material 0 is the renderer's default; ids start after it.
  uint32_t next_mesh = 0;
  uint32_t next_texture = 0;
  uint32_t next_material = 1;
  int mesh_uploads = 0;
  int texture_uploads = 0;
  std::vector<material::MaterialDesc> descs;  // per created material
  std::vector<render::MaterialTextures> texture_sets;
  std::vector<std::pair<render::MaterialId, material::MaterialDesc>> updates;
  std::vector<render::MeshId> released_meshes;
  std::vector<render::TextureId> released_textures;
  std::vector<render::MaterialId> released_materials;
};

// Returns a prefab with one unit cube mesh on a root node, using one
// material per name in `materials` (one submesh each, splitting the cube's
// indices), and a box collider when `collider` is set.
inline prefab::Prefab MakeCubePrefab(std::vector<std::string> materials,
                                     bool collider = false) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  const asset::MeshData cube = asset::MakeCube();
  prefab::Mesh mesh{.name = "cube", .node = 0, .bounds = cube.bounds};
  mesh.vertices =
      prefab.storage.StoreArray<asset::Vertex>(std::span(cube.vertices));
  mesh.indices =
      prefab.storage.StoreArray<asset::Index>(std::span(cube.indices));
  const auto total = static_cast<uint32_t>(cube.indices.size());
  const auto count = static_cast<uint32_t>(materials.size());
  const uint32_t per = total / 3 / count * 3;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t first = i * per;
    mesh.submeshes.push_back(
        {.first_index = first,
         .index_count = i + 1 == count ? total - first : per,
         .material = static_cast<int>(i)});
    prefab.materials.push_back({.name = materials[i]});
  }
  prefab.meshes.push_back(std::move(mesh));
  prefab.bounds = cube.bounds;
  if (collider) {
    prefab.physics.kind = prefab::ColliderKind::kBox;
    prefab.physics.half_extents = glm::vec3(0.5f);
  }
  return prefab;
}

}  // namespace blue::testing

#endif  // BLUE_TESTS_WORLD_FIXTURES_H_
