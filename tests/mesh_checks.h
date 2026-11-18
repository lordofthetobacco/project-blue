#ifndef BLUE_TESTS_MESH_CHECKS_H_
#define BLUE_TESTS_MESH_CHECKS_H_

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>

#include <glm/geometric.hpp>

#include "asset/mesh_data.h"

// Shared doctest checks for asset::MeshData vertex frames and submeshes.

namespace blue::testing {

// Tolerance for unit lengths and orthogonality.
inline constexpr float kFrameEpsilon = 1.0e-3f;

// Returns the fraction of `mesh`'s triangles with a non-degenerate UV
// mapping whose vertex tangent frames agree with the triangle's own UV
// directions: tangent along +u and cross(normal, tangent) * w along +v.
// Returns 1 if no triangle has a usable UV mapping.
inline float TangentAgreement(const asset::MeshData& mesh) {
  size_t usable = 0;
  size_t agreeing = 0;
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const asset::Vertex& a = mesh.vertices[mesh.indices[i]];
    const asset::Vertex& b = mesh.vertices[mesh.indices[i + 1]];
    const asset::Vertex& c = mesh.vertices[mesh.indices[i + 2]];
    const glm::vec3 e1 = b.position - a.position;
    const glm::vec3 e2 = c.position - a.position;
    const glm::vec2 d1 = b.uv - a.uv;
    const glm::vec2 d2 = c.uv - a.uv;
    const float determinant = d1.x * d2.y - d2.x * d1.y;
    if (std::abs(determinant) < 1.0e-10f ||
        glm::length(glm::cross(e1, e2)) < 1.0e-10f) {
      continue;
    }
    const glm::vec3 dp_du = (e1 * d2.y - e2 * d1.y) / determinant;
    const glm::vec3 dp_dv = (e2 * d1.x - e1 * d2.x) / determinant;
    ++usable;
    bool agrees = true;
    for (const asset::Vertex* vertex : {&a, &b, &c}) {
      const glm::vec3 tangent(vertex->tangent);
      const glm::vec3 bitangent =
          glm::cross(vertex->normal, tangent) * vertex->tangent.w;
      agrees = agrees && glm::dot(tangent, dp_du) > 0.0f &&
               glm::dot(bitangent, dp_dv) > 0.0f;
    }
    agreeing += agrees ? 1 : 0;
  }
  return usable == 0
             ? 1.0f
             : static_cast<float>(agreeing) / static_cast<float>(usable);
}

// Checks that every vertex of `mesh` has a unit normal, a unit tangent
// orthogonal to it and handedness +-1, and, unless `min_agreement` is lower,
// that every triangle's tangent frames follow its UVs.
inline void CheckTangentFrames(const asset::MeshData& mesh,
                               float min_agreement = 1.0f) {
  size_t bad_frames = 0;
  for (const asset::Vertex& vertex : mesh.vertices) {
    const glm::vec3 tangent(vertex.tangent);
    const bool good =
        std::abs(glm::length(vertex.normal) - 1.0f) <= kFrameEpsilon &&
        std::abs(glm::length(tangent) - 1.0f) <= kFrameEpsilon &&
        std::abs(glm::dot(vertex.normal, tangent)) <= kFrameEpsilon &&
        std::abs(vertex.tangent.w) == 1.0f;
    bad_frames += good ? 0 : 1;
  }
  CHECK(bad_frames == 0);
  CHECK(TangentAgreement(mesh) >= min_agreement);
}

// Checks that `mesh` has exactly one submesh, material 0, covering every
// index.
inline void CheckSingleSubmesh(const asset::MeshData& mesh) {
  REQUIRE(mesh.submeshes.size() == 1);
  CHECK(mesh.submeshes[0].first_index == 0);
  CHECK(mesh.submeshes[0].index_count == mesh.indices.size());
  CHECK(mesh.submeshes[0].material == 0);
}

// Checks that the submeshes of `mesh` cover its indices in order without
// gaps, each a whole number of triangles, with material indices below
// `material_count`.
inline void CheckSubmeshes(const asset::MeshData& mesh, size_t material_count) {
  size_t next = 0;
  for (const asset::Submesh& submesh : mesh.submeshes) {
    CHECK(submesh.first_index == next);
    CHECK(submesh.index_count % 3 == 0);
    CHECK(submesh.index_count > 0);
    CHECK(submesh.material >= 0);
    CHECK(static_cast<size_t>(submesh.material) < material_count);
    next += submesh.index_count;
  }
  CHECK(next == mesh.indices.size());
}

}  // namespace blue::testing

#endif  // BLUE_TESTS_MESH_CHECKS_H_
