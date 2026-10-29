#include "asset/mesh_data.h"

#include <array>

#include <glm/geometric.hpp>

namespace blue::asset {

MeshData MakeCube(glm::vec3 color) {
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
    // from outside.
    const glm::vec3 right = glm::cross(face.up, face.normal);
    const glm::vec3 center = face.normal * 0.5f;
    const glm::vec3 r = right * 0.5f;
    const glm::vec3 u = face.up * 0.5f;

    const auto base = static_cast<Index>(mesh.vertices.size());
    mesh.vertices.push_back({center - r - u, face.normal, color});
    mesh.vertices.push_back({center + r - u, face.normal, color});
    mesh.vertices.push_back({center + r + u, face.normal, color});
    mesh.vertices.push_back({center - r + u, face.normal, color});

    constexpr std::array<Index, 6> kQuadIndices = {0, 1, 2, 2, 3, 0};
    for (const Index offset : kQuadIndices) {
      mesh.indices.push_back(base + offset);
    }
  }
  mesh.bounds = {.min = glm::vec3(-0.5f), .max = glm::vec3(0.5f)};
  return mesh;
}

MeshData MakePlane(float size, glm::vec3 color) {
  const float h = size * 0.5f;
  constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};

  MeshData mesh;
  mesh.vertices = {
      {{-h, 0.0f, -h}, kUp, color},
      {{-h, 0.0f, h}, kUp, color},
      {{h, 0.0f, h}, kUp, color},
      {{h, 0.0f, -h}, kUp, color},
  };
  mesh.indices = {0, 1, 2, 2, 3, 0};
  mesh.bounds = {.min = {-h, 0.0f, -h}, .max = {h, 0.0f, h}};
  return mesh;
}

}  // namespace blue::asset
