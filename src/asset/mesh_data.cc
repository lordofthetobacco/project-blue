#include "asset/mesh_data.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace blue::asset {
namespace {

// Returns the single submesh covering all of `mesh`'s indices.
std::vector<Submesh> WholeMesh(const MeshData& mesh) {
  return {{.first_index = 0,
           .index_count = static_cast<uint32_t>(mesh.indices.size()),
           .material = 0}};
}

}  // namespace

MeshData MakeCube() {
  struct Face {
    glm::vec3 normal;
    glm::vec3 up;  // Any direction perpendicular to the normal.
  };
  constexpr std::array<Face, 6> kFaces = {{
      {{1, 0, 0}, {0, 1, 0}},
      {{-1, 0, 0}, {0, 1, 0}},
      {{0, 0, 1}, {0, 1, 0}},
      {{0, 0, -1}, {0, 1, 0}},
      {{0, 1, 0}, {0, 0, -1}},
      {{0, -1, 0}, {0, 0, 1}},
  }};

  MeshData mesh;
  mesh.vertices.reserve(kFaces.size() * 4);
  mesh.indices.reserve(kFaces.size() * 6);

  for (const Face& face : kFaces) {
    // right x up == normal, so walking right -> up is counter-clockwise seen
    // from outside. u grows along `right` and v along -up (v points down the
    // image), so cross(normal, right) == up == -bitangent: handedness -1.
    const glm::vec3 right = glm::cross(face.up, face.normal);
    const glm::vec3 center = face.normal * 0.5f;
    const glm::vec3 r = right * 0.5f;
    const glm::vec3 u = face.up * 0.5f;
    const glm::vec4 tangent(right, -1.0f);

    const auto base = static_cast<Index>(mesh.vertices.size());
    mesh.vertices.push_back(
        {center - r - u, face.normal, {0, 1}, {0, 1}, tangent});
    mesh.vertices.push_back(
        {center + r - u, face.normal, {1, 1}, {1, 1}, tangent});
    mesh.vertices.push_back(
        {center + r + u, face.normal, {1, 0}, {1, 0}, tangent});
    mesh.vertices.push_back(
        {center - r + u, face.normal, {0, 0}, {0, 0}, tangent});

    constexpr std::array<Index, 6> kQuadIndices = {0, 1, 2, 2, 3, 0};
    for (const Index offset : kQuadIndices) {
      mesh.indices.push_back(base + offset);
    }
  }
  mesh.submeshes = WholeMesh(mesh);
  mesh.bounds = {.min = glm::vec3(-0.5f), .max = glm::vec3(0.5f)};
  return mesh;
}

MeshData MakePlane(float size) {
  const float h = size * 0.5f;
  constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};
  // u along +X, v along +Z; cross(+Y, +X) = -Z = -bitangent: handedness -1.
  constexpr glm::vec4 kTangent{1.0f, 0.0f, 0.0f, -1.0f};

  MeshData mesh;
  mesh.vertices = {
      {{-h, 0.0f, -h}, kUp, {0.0f, 0.0f}, {0.0f, 0.0f}, kTangent},
      {{-h, 0.0f, h}, kUp, {0.0f, size}, {0.0f, size}, kTangent},
      {{h, 0.0f, h}, kUp, {size, size}, {size, size}, kTangent},
      {{h, 0.0f, -h}, kUp, {size, 0.0f}, {size, 0.0f}, kTangent},
  };
  mesh.indices = {0, 1, 2, 2, 3, 0};
  mesh.submeshes = WholeMesh(mesh);
  mesh.bounds = {.min = {-h, 0.0f, -h}, .max = {h, 0.0f, h}};
  return mesh;
}

MeshData MakeSphere(int segments) {
  constexpr float kRadius = 0.5f;
  constexpr float kPi = std::numbers::pi_v<float>;
  const int slices = std::max(segments, 3);
  const int stacks = std::max(slices / 2, 2);

  MeshData mesh;
  mesh.vertices.reserve(static_cast<size_t>((slices + 1) * (stacks + 1)));
  mesh.indices.reserve(static_cast<size_t>(slices * stacks * 6));

  // Ring `stack` of (stacks + 1) runs from the north pole (v = 0) to the
  // south pole (v = 1); column `slice` of (slices + 1) repeats the seam.
  for (int stack = 0; stack <= stacks; ++stack) {
    const float v = static_cast<float>(stack) / static_cast<float>(stacks);
    const float polar = v * kPi;
    for (int slice = 0; slice <= slices; ++slice) {
      const float u = static_cast<float>(slice) / static_cast<float>(slices);
      const float azimuth = u * 2.0f * kPi;
      const float sin_polar = std::sin(polar);
      const float cos_polar = std::cos(polar);
      const float sin_azimuth = std::sin(azimuth);
      const float cos_azimuth = std::cos(azimuth);
      // u = 0 faces +Z; seen from outside with north up, u grows to the
      // right. The tangent is d(normal)/d(azimuth) normalized, which stays
      // well defined at the poles.
      const glm::vec3 normal{sin_polar * sin_azimuth, cos_polar,
                             sin_polar * cos_azimuth};
      const glm::vec3 tangent{cos_azimuth, 0.0f, -sin_azimuth};
      // +v points south: d(normal)/d(polar).
      const glm::vec3 south{cos_polar * sin_azimuth, -sin_polar,
                            cos_polar * cos_azimuth};
      const float w =
          glm::dot(glm::cross(normal, tangent), south) >= 0.0f ? 1.0f : -1.0f;
      mesh.vertices.push_back(
          {normal * kRadius, normal, {u, v}, {u, v}, glm::vec4(tangent, w)});
    }
  }
  const auto row = static_cast<Index>(slices + 1);
  for (int stack = 0; stack < stacks; ++stack) {
    for (int slice = 0; slice < slices; ++slice) {
      const auto a = static_cast<Index>(stack) * row +
                     static_cast<Index>(slice);  // top-left
      const Index b = a + row;                   // bottom-left
      // Seen from outside with north up, u grows to the right, so
      // a -> b -> b + 1 is counter-clockwise. Skip the degenerate triangle at
      // each pole.
      if (stack != 0) {
        mesh.indices.insert(mesh.indices.end(), {a, b, a + 1});
      }
      if (stack != stacks - 1) {
        mesh.indices.insert(mesh.indices.end(), {a + 1, b, b + 1});
      }
    }
  }
  mesh.submeshes = WholeMesh(mesh);
  mesh.bounds = {.min = glm::vec3(-kRadius), .max = glm::vec3(kRadius)};
  return mesh;
}

MeshData MakeCapsule(float radius, float height, int segments) {
  constexpr float kPi = std::numbers::pi_v<float>;
  const float r = std::max(radius, 1.0e-4f);
  const float total = std::max(height, 2.0f * r);
  const int slices = std::max(segments, 3);
  int stacks = std::max(slices / 2, 2);
  stacks += stacks % 2;
  const int half = stacks / 2;

  // Rings from the top pole down: the upper hemisphere's (centred at
  // total - r), then the lower one's (centred at r), the equator twice.
  struct Ring {
    float polar = 0.0f;
    float centre = 0.0f;
  };
  std::vector<Ring> rings;
  rings.reserve(static_cast<size_t>(stacks + 2));
  for (int stack = 0; stack <= half; ++stack) {
    rings.push_back(
        {.polar = static_cast<float>(stack) / static_cast<float>(stacks) * kPi,
         .centre = total - r});
  }
  for (int stack = half; stack <= stacks; ++stack) {
    rings.push_back(
        {.polar = static_cast<float>(stack) / static_cast<float>(stacks) * kPi,
         .centre = r});
  }

  MeshData mesh;
  const auto ring_count = static_cast<int>(rings.size());
  mesh.vertices.reserve(static_cast<size_t>((slices + 1) * ring_count));
  mesh.indices.reserve(static_cast<size_t>(slices * (ring_count - 1) * 6));
  for (int k = 0; k < ring_count; ++k) {
    const Ring& ring = rings[static_cast<size_t>(k)];
    const float v = static_cast<float>(k) / static_cast<float>(ring_count - 1);
    const float sin_polar = std::sin(ring.polar);
    const float cos_polar = std::cos(ring.polar);
    for (int slice = 0; slice <= slices; ++slice) {
      const float u = static_cast<float>(slice) / static_cast<float>(slices);
      const float azimuth = u * 2.0f * kPi;
      const float sin_azimuth = std::sin(azimuth);
      const float cos_azimuth = std::cos(azimuth);
      const glm::vec3 normal{sin_polar * sin_azimuth, cos_polar,
                             sin_polar * cos_azimuth};
      const glm::vec3 tangent{cos_azimuth, 0.0f, -sin_azimuth};
      const glm::vec3 south{cos_polar * sin_azimuth, -sin_polar,
                            cos_polar * cos_azimuth};
      const float w =
          glm::dot(glm::cross(normal, tangent), south) >= 0.0f ? 1.0f : -1.0f;
      const glm::vec3 position =
          normal * r + glm::vec3(0.0f, ring.centre, 0.0f);
      mesh.vertices.push_back(
          {position, normal, {u, v}, {u, v}, glm::vec4(tangent, w)});
    }
  }
  const auto row = static_cast<Index>(slices + 1);
  for (int k = 0; k + 1 < ring_count; ++k) {
    for (int slice = 0; slice < slices; ++slice) {
      const auto a = static_cast<Index>(k) * row + static_cast<Index>(slice);
      const Index b = a + row;
      // As MakeSphere: counter-clockwise from outside, no degenerate
      // triangles at the poles.
      if (k != 0) {
        mesh.indices.insert(mesh.indices.end(), {a, b, a + 1});
      }
      if (k != ring_count - 2) {
        mesh.indices.insert(mesh.indices.end(), {a + 1, b, b + 1});
      }
    }
  }
  mesh.submeshes = WholeMesh(mesh);
  mesh.bounds = {.min = glm::vec3(-r, 0.0f, -r), .max = glm::vec3(r, total, r)};
  return mesh;
}

Bounds ComputeBounds(const std::vector<Vertex>& vertices) {
  if (vertices.empty()) {
    return {};
  }
  Bounds bounds{.min = vertices.front().position,
                .max = vertices.front().position};
  for (const Vertex& vertex : vertices) {
    bounds.min = glm::min(bounds.min, vertex.position);
    bounds.max = glm::max(bounds.max, vertex.position);
  }
  return bounds;
}

}  // namespace blue::asset
