#ifndef BLUE_ASSET_MESH_DATA_H_
#define BLUE_ASSET_MESH_DATA_H_

#include <cstdint>
#include <vector>

#include <glm/vec3.hpp>

namespace blue::asset {

// One mesh vertex. The layout is uploaded to the GPU as is, so the field
// order and types must match the vertex input description in the renderer.
struct Vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec3 color;
};

// 32-bit so real models (> 65k vertices) fit.
using Index = uint32_t;

// Axis-aligned bounding box in the mesh's local space.
struct Bounds {
  glm::vec3 min{0.0f};
  glm::vec3 max{0.0f};

  [[nodiscard]] glm::vec3 size() const { return max - min; }
  [[nodiscard]] glm::vec3 center() const { return (min + max) * 0.5f; }
};

// CPU-side triangle mesh: every three consecutive `indices` form one
// triangle, and `bounds` encloses all vertices.
struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<Index> indices;
  Bounds bounds;
};

// Returns a unit cube centred at the origin. Each face has its own four
// vertices so it gets a flat normal; triangles are counter-clockwise when
// seen from outside.
[[nodiscard]] MeshData MakeCube(glm::vec3 color);

// Returns a square of side `size` in the XZ plane at y = 0, centred at the
// origin, with normal +Y and counter-clockwise winding seen from above.
[[nodiscard]] MeshData MakePlane(float size, glm::vec3 color);

}  // namespace blue::asset

#endif  // BLUE_ASSET_MESH_DATA_H_
