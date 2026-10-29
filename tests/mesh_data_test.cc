#include "asset/mesh_data.h"

#include <doctest/doctest.h>

#include <glm/geometric.hpp>

namespace blue::asset {

TEST_CASE("MakePlane builds one upward-facing quad") {
  const glm::vec3 color{0.1f, 0.2f, 0.3f};
  const MeshData plane = MakePlane(12.0f, color);

  REQUIRE(plane.vertices.size() == 4);
  REQUIRE(plane.indices.size() == 6);
  CHECK(plane.bounds.min == glm::vec3(-6.0f, 0.0f, -6.0f));
  CHECK(plane.bounds.max == glm::vec3(6.0f, 0.0f, 6.0f));

  for (const Vertex& vertex : plane.vertices) {
    CHECK(vertex.normal == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(vertex.color == color);
    CHECK(vertex.position.y == 0.0f);
  }
  for (Index index : plane.indices) {
    CHECK(index < plane.vertices.size());
  }
}

TEST_CASE("MakePlane triangles are counter-clockwise seen from above") {
  const MeshData plane = MakePlane(2.0f, glm::vec3(1.0f));

  for (size_t i = 0; i < plane.indices.size(); i += 3) {
    const glm::vec3& a = plane.vertices[plane.indices[i]].position;
    const glm::vec3& b = plane.vertices[plane.indices[i + 1]].position;
    const glm::vec3& c = plane.vertices[plane.indices[i + 2]].position;
    CHECK(glm::cross(b - a, c - a).y > 0.0f);
  }
}

TEST_CASE("MakeCube is a closed cube with outward counter-clockwise faces") {
  const MeshData cube = MakeCube({1.0f, 0.0f, 0.0f});
  REQUIRE(cube.vertices.size() == 24);
  REQUIRE(cube.indices.size() == 36);

  for (size_t i = 0; i < cube.indices.size(); i += 3) {
    const Vertex& a = cube.vertices[cube.indices[i]];
    const Vertex& b = cube.vertices[cube.indices[i + 1]];
    const Vertex& c = cube.vertices[cube.indices[i + 2]];
    const glm::vec3 geometric =
        glm::cross(b.position - a.position, c.position - a.position);
    CHECK(glm::dot(geometric, a.normal) > 0.0f);
  }
}

}  // namespace blue::asset
