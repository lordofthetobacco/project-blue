#ifndef BLUE_ASSET_MESH_DATA_H_
#define BLUE_ASSET_MESH_DATA_H_

#include <cstdint>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace blue::asset {

// One mesh vertex, 48 bytes. The layout is uploaded to the GPU as is, so the
// field order and types must match the vertex input description in the
// renderer. Colour is not a vertex attribute: it comes from the material.
//
// `uv` follows the glTF / Vulkan convention: (0, 0) is the top-left corner of
// the image and v grows downwards. `tangent.xyz` is a unit vector orthogonal
// to `normal` pointing along +u; `tangent.w` is the handedness (+1 or -1) so
// that the bitangent (+v direction) is cross(normal, tangent.xyz) * w.
struct Vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec2 uv;
  glm::vec4 tangent;
};

// 32-bit so real models (> 65k vertices) fit.
using Index = uint32_t;

// A contiguous range of MeshData::indices drawn with one material.
// `material` indexes the material table of the model that owns the mesh
// (asset::Model::materials or AnimatedModel::materials); built-in meshes use
// material 0 of whatever the caller assigns.
struct Submesh {
  uint32_t first_index = 0;  // offset into MeshData::indices
  uint32_t index_count = 0;  // multiple of 3
  int material = 0;
};

// Axis-aligned bounding box in the mesh's local space.
struct Bounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};

  [[nodiscard]] glm::vec3 size() const { return max - min; }
  [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
};

// CPU-side triangle mesh: every three consecutive `indices` form one
// triangle, and `bounds` encloses all vertices. `submeshes` partition
// `indices` into material ranges, in index order, covering every index
// exactly once.
struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<Index> indices;
  std::vector<Submesh> submeshes;
  Bounds bounds;
};

// Returns a unit cube centred at the origin. Each face has its own four
// vertices so it gets a flat normal and UVs spanning 0..1; triangles are
// counter-clockwise when seen from outside. One submesh, material 0.
[[nodiscard]] MeshData MakeCube();

// Returns a square of side `size` in the XZ plane at y = 0, centred at the
// origin, with normal +Y and counter-clockwise winding seen from above. UVs
// are in metres: u = x + size / 2 and v = z + size / 2, so a material's
// uv_scale is "texture repeats per metre" (uv_scale 0.5 = one tile per 2 m).
// Tangent +X. One submesh, material 0.
[[nodiscard]] MeshData MakePlane(float size);

// Returns a UV sphere of radius 0.5 centred at the origin with smooth normals,
// `segments` slices around Y (clamped to >= 3) and segments / 2 stacks (>= 2).
// u runs once around (seam duplicated, u = 0 facing +Z, growing to the right
// seen from outside), v = 0 at the north pole (+Y). Tangents point along +u;
// at the poles they continue the neighbouring rings. One submesh, material 0.
[[nodiscard]] MeshData MakeSphere(int segments);

// Returns the box enclosing `vertices`, or an empty box at the origin.
[[nodiscard]] Bounds ComputeBounds(const std::vector<Vertex>& vertices);

}  // namespace blue::asset

#endif  // BLUE_ASSET_MESH_DATA_H_
