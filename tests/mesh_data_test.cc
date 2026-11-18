#include "asset/mesh_data.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>

#include <glm/geometric.hpp>
#include <glm/vector_relational.hpp>

#include "tests/mesh_checks.h"

namespace blue::asset {

TEST_CASE("Vertex has the 56-byte layout the renderer uploads") {
  CHECK(sizeof(Vertex) == 56);
  CHECK(offsetof(Vertex, position) == 0);
  CHECK(offsetof(Vertex, normal) == 12);
  CHECK(offsetof(Vertex, uv0) == 24);
  CHECK(offsetof(Vertex, uv1) == 32);
  CHECK(offsetof(Vertex, tangent) == 40);
}

TEST_CASE("MakePlane builds one upward-facing quad") {
  const MeshData plane = MakePlane(12.0f);

  REQUIRE(plane.vertices.size() == 4);
  REQUIRE(plane.indices.size() == 6);
  CHECK(plane.bounds.min == glm::vec3(-6.0f, 0.0f, -6.0f));
  CHECK(plane.bounds.max == glm::vec3(6.0f, 0.0f, 6.0f));
  testing::CheckSingleSubmesh(plane);

  for (const Vertex& vertex : plane.vertices) {
    CHECK(vertex.normal == glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK(vertex.position.y == 0.0f);
    // UVs are in metres from the -X/-Z corner.
    CHECK(vertex.uv0.x == doctest::Approx(vertex.position.x + 6.0f));
    CHECK(vertex.uv0.y == doctest::Approx(vertex.position.z + 6.0f));
  }
  for (Index index : plane.indices) {
    CHECK(index < plane.vertices.size());
  }
  testing::CheckTangentFrames(plane);
}

TEST_CASE("MakePlane triangles are counter-clockwise seen from above") {
  const MeshData plane = MakePlane(2.0f);

  for (size_t i = 0; i < plane.indices.size(); i += 3) {
    const glm::vec3& a = plane.vertices[plane.indices[i]].position;
    const glm::vec3& b = plane.vertices[plane.indices[i + 1]].position;
    const glm::vec3& c = plane.vertices[plane.indices[i + 2]].position;
    CHECK(glm::cross(b - a, c - a).y > 0.0f);
  }
}

TEST_CASE("MakeCube is a closed cube with outward counter-clockwise faces") {
  const MeshData cube = MakeCube();
  REQUIRE(cube.vertices.size() == 24);
  REQUIRE(cube.indices.size() == 36);
  testing::CheckSingleSubmesh(cube);

  for (size_t i = 0; i < cube.indices.size(); i += 3) {
    const Vertex& a = cube.vertices[cube.indices[i]];
    const Vertex& b = cube.vertices[cube.indices[i + 1]];
    const Vertex& c = cube.vertices[cube.indices[i + 2]];
    const glm::vec3 geometric =
        glm::cross(b.position - a.position, c.position - a.position);
    CHECK(glm::dot(geometric, a.normal) > 0.0f);
  }
  for (const Vertex& vertex : cube.vertices) {
    CHECK(glm::all(glm::greaterThanEqual(vertex.uv0, glm::vec2(0.0f))));
    CHECK(glm::all(glm::lessThanEqual(vertex.uv0, glm::vec2(1.0f))));
  }
  testing::CheckTangentFrames(cube);
}

TEST_CASE("Tangent frames follow u to the right and v down the image") {
  // A quad seen from +Z (its normal): u grows to the right (+X) and v grows
  // downwards (-Y), like an image laid on it.
  MeshData quad;
  constexpr glm::vec3 kNormal{0.0f, 0.0f, 1.0f};
  constexpr glm::vec4 kTangent{1.0f, 0.0f, 0.0f, -1.0f};
  quad.vertices = {
      {{0, 1, 0}, kNormal, {0, 0}, {0, 0}, kTangent},
      {{0, 0, 0}, kNormal, {0, 1}, {0, 1}, kTangent},
      {{1, 0, 0}, kNormal, {1, 1}, {1, 1}, kTangent},
  };
  quad.indices = {0, 1, 2};
  testing::CheckTangentFrames(quad);
  // The bitangent cross(N, T) * w is the +v direction: image down = -Y.
  const glm::vec3 bitangent =
      glm::cross(kNormal, glm::vec3(kTangent)) * kTangent.w;
  CHECK(bitangent == glm::vec3(0.0f, -1.0f, 0.0f));
}

TEST_CASE("MakeSphere is a closed sphere with outward faces and UVs") {
  const MeshData sphere = MakeSphere(32);
  REQUIRE_FALSE(sphere.vertices.empty());
  testing::CheckSingleSubmesh(sphere);
  CHECK(sphere.bounds.min == glm::vec3(-0.5f));
  CHECK(sphere.bounds.max == glm::vec3(0.5f));
  // 32 slices x 16 stacks, minus one triangle per slice at each pole.
  CHECK(sphere.indices.size() == size_t{32} * (16 * 2 - 2) * 3);

  for (const Vertex& vertex : sphere.vertices) {
    CHECK(glm::length(vertex.position) == doctest::Approx(0.5f));
    CHECK(glm::all(glm::lessThanEqual(
        glm::abs(vertex.normal - vertex.position * 2.0f), glm::vec3(1e-5f))));
    CHECK(vertex.uv0.x >= 0.0f);
    CHECK(vertex.uv0.x <= 1.0f);
    // v = 0 at the north pole.
    CHECK(vertex.uv0.y ==
          doctest::Approx(std::acos(vertex.normal.y) / 3.14159265f)
              .epsilon(1e-4));
  }
  for (size_t i = 0; i < sphere.indices.size(); i += 3) {
    const Vertex& a = sphere.vertices[sphere.indices[i]];
    const Vertex& b = sphere.vertices[sphere.indices[i + 1]];
    const Vertex& c = sphere.vertices[sphere.indices[i + 2]];
    const glm::vec3 geometric =
        glm::cross(b.position - a.position, c.position - a.position);
    CHECK(glm::length(geometric) > 0.0f);
    CHECK(glm::dot(geometric, a.position + b.position + c.position) > 0.0f);
  }
  testing::CheckTangentFrames(sphere);
}

TEST_CASE("MakeSphere clamps tiny segment counts") {
  const MeshData sphere = MakeSphere(0);
  CHECK(sphere.vertices.size() == 4 * 3);  // 3 slices + seam, 2 stacks + 1
  CHECK_FALSE(sphere.indices.empty());
}

TEST_CASE("ComputeBounds encloses every vertex") {
  CHECK(ComputeBounds({}).min == glm::vec3(0.0f));
  const MeshData cube = MakeCube();
  const Bounds bounds = ComputeBounds(cube.vertices);
  CHECK(bounds.min == glm::vec3(-0.5f));
  CHECK(bounds.max == glm::vec3(0.5f));
}

TEST_CASE("Built-in meshes copy uv0 into uv1") {
  for (const MeshData& mesh : {MakeCube(), MakePlane(3.0f), MakeSphere(8)}) {
    for (const Vertex& vertex : mesh.vertices) {
      CHECK(vertex.uv1 == vertex.uv0);
    }
  }
}

}  // namespace blue::asset
