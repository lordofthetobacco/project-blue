#ifndef BLUE_TESTS_PREFAB_SAMPLES_H_
#define BLUE_TESTS_PREFAB_SAMPLES_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "asset/mesh_data.h"
#include "prefab/prefab.h"

namespace blue::testing {

// Returns `count` bytes of a recognisable pattern starting at `seed`.
inline std::vector<std::byte> PatternBytes(size_t count, uint32_t seed) {
  std::vector<std::byte> bytes(count);
  uint32_t state = seed * 2654435761U + 1U;
  for (std::byte& b : bytes) {
    state = state * 1664525U + 1013904223U;
    b = static_cast<std::byte>(state >> 24);
  }
  return bytes;
}

// Returns a texture of `width` x `height` in `format` with a full mip chain
// of pattern bytes stored in `prefab`.
inline prefab::Texture MakePatternTexture(prefab::Prefab& prefab,
                                          std::string name,
                                          prefab::TextureFormat format,
                                          int width, int height,
                                          uint32_t seed) {
  prefab::Texture texture{.name = std::move(name),
                          .format = format,
                          .width = width,
                          .height = height};
  const int levels = prefab::FullMipCount(width, height);
  for (int level = 0; level < levels; ++level) {
    const int w = std::max(1, width >> level);
    const int h = std::max(1, height >> level);
    const std::span<const std::byte> data =
        prefab.storage.Adopt(PatternBytes(prefab::MipByteSize(format, w, h),
                                          seed + static_cast<uint32_t>(level)));
    texture.mips.push_back({.width = w, .height = h, .data = data});
  }
  return texture;
}

// Returns a valid prefab that uses every chunk type: two nodes, a cube mesh
// with two submeshes, two materials, two textures (BC7 sRGB, BC5), a looping
// clip with events, a socket, a light, a child prefab, a cooked physics
// shape, two tags and edit info. Paths are relative so the prefab can be
// encoded with an empty base directory.
inline prefab::Prefab MakeSamplePrefab() {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {
      {.name = "root", .parent = -1, .rest = {}},
      {.name = "slide",
       .parent = 0,
       .rest = {.translation = {0.0f, 0.1f, 0.2f},
                .rotation = {0.5f, 0.5f, 0.5f, 0.5f},
                .scale = {1.0f, 2.0f, 1.0f}}},
  };

  const asset::MeshData cube = asset::MakeCube();
  prefab::Mesh mesh;
  mesh.name = "body";
  mesh.node = 1;
  mesh.bounds = cube.bounds;
  mesh.vertices =
      prefab.storage.StoreArray<asset::Vertex>(std::span(cube.vertices));
  mesh.indices =
      prefab.storage.StoreArray<asset::Index>(std::span(cube.indices));
  const auto half = static_cast<uint32_t>(cube.indices.size() / 2);
  mesh.submeshes = {
      {.first_index = 0, .index_count = half, .material = 0},
      {.first_index = half,
       .index_count = static_cast<uint32_t>(cube.indices.size()) - half,
       .material = 1}};
  prefab.meshes.push_back(mesh);
  prefab.bounds = cube.bounds;

  prefab.textures.push_back(MakePatternTexture(
      prefab, "albedo", prefab::TextureFormat::kBc7Srgb, 20, 12, 1));
  prefab.textures.push_back(MakePatternTexture(
      prefab, "normal", prefab::TextureFormat::kBc5Unorm, 8, 8, 2));

  prefab::Material steel;
  steel.name = "steel";
  steel.base_color = {0.5f, 0.6f, 0.7f, 1.0f};
  steel.metallic = 1.0f;
  steel.roughness = 0.3f;
  steel.emissive = {0.1f, 0.0f, 0.0f};
  steel.emissive_strength = 2.0f;
  steel.normal_scale = 0.8f;
  steel.occlusion_strength = 0.5f;
  steel.uv_scale = {2.0f, 3.0f};
  steel.alpha_mode = material::AlphaMode::kMask;
  steel.alpha_cutoff = 0.25f;
  steel.double_sided = true;
  steel.textures = {0, -1, 1, -1};
  steel.uv_sets = {0, 0, 1, 0};
  prefab.materials.push_back(steel);
  prefab.materials.push_back({.name = "grip"});

  animation::Clip fire{.name = "fire", .duration = 0.5f, .loop = true};
  fire.channels.push_back({.node = 1,
                           .path = animation::TargetPath::kTranslation,
                           .interpolation = animation::Interpolation::kLinear,
                           .times = {0.0f, 0.5f},
                           .values = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -0.05f}});
  animation::AddEvent(fire, {.name = "shot", .time = 0.0f});
  animation::AddEvent(fire, {.name = "slide_lock", .time = 0.2f});
  prefab.clips.push_back(fire);

  prefab.sockets.push_back(
      {.name = "muzzle", .node = 1, .local = {.translation = {0, 0, -0.3f}}});
  prefab.lights.push_back({.name = "lamp",
                           .node = 0,
                           .position = {1.0f, 2.0f, 3.0f},
                           .color = {1.0f, 0.9f, 0.8f},
                           .intensity = 5.0f,
                           .range = 4.0f});
  prefab.children.push_back(
      {.path = "parts/sight.pbo", .node = 1, .local = {.scale = {2, 2, 2}}});

  prefab.physics.kind = prefab::ColliderKind::kConvexHull;
  prefab.physics.center = {0.0f, 0.5f, 0.0f};
  prefab.physics.dynamic = true;
  prefab.physics.mass = 3.0f;
  prefab.physics.friction = 0.7f;
  prefab.physics.restitution = 0.1f;
  prefab.physics.cooked_shape = prefab.storage.Adopt(PatternBytes(77, 9));

  prefab.tags = {{.key = "category", .value = "test"},
                 {.key = "note.v2", .value = "free text = ok"}};
  prefab.edit.source_model = "sources/glock.gltf";
  prefab.edit.import_settings = "scale=1\nmerge_static=0\n";
  prefab.edit.texture_sources = {{"sources/albedo.png"},
                                 {"sources/normal.png", "sources/n2.png"}};
  return prefab;
}

}  // namespace blue::testing

#endif  // BLUE_TESTS_PREFAB_SAMPLES_H_
