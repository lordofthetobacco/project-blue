#include "asset/internal/mesh_import.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

#include <glm/geometric.hpp>

#include "core/log.h"

namespace blue::asset::internal {
namespace {

// Tangents shorter than this after orthogonalization are degenerate (UVs
// collapsed along an edge, or assimp's NaN for missing UVs).
constexpr float kMinTangentLength = 1.0e-6f;

glm::vec3 ToVec3(const aiVector3D& v) { return {v.x, v.y, v.z}; }

// Returns a unit tangent perpendicular to `normal` that depends only on
// `normal`, with handedness -1 like the built-in meshes.
glm::vec4 FallbackTangent(glm::vec3 normal) {
  const glm::vec3 axis =
      std::abs(normal.y) < 0.999f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  return {glm::normalize(glm::cross(axis, normal)), -1.0f};
}

// Returns the tangent of vertex `i` of `mesh` for unit `normal`.
glm::vec4 VertexTangent(const aiMesh& mesh, unsigned i, glm::vec3 normal) {
  if (mesh.mTangents == nullptr || mesh.mBitangents == nullptr) {
    return FallbackTangent(normal);
  }
  const glm::vec3 raw = ToVec3(mesh.mTangents[i]);
  const glm::vec3 tangent = raw - normal * glm::dot(normal, raw);
  const float length = glm::length(tangent);
  if (!std::isfinite(length) || length < kMinTangentLength) {
    return FallbackTangent(normal);
  }
  const glm::vec3 unit = tangent / length;
  // Despite its comment, aiProcess_CalcTangentSpace computes the bitangent as
  // -dP/dv of assimp's bottom-up v (CalcTangentsProcess.cpp: the sign
  // correction is applied to dP/dv * det), which is +dP/dv of the flipped,
  // top-down v used here.
  const glm::vec3 down_bitangent = ToVec3(mesh.mBitangents[i]);
  const float w =
      glm::dot(glm::cross(normal, unit), down_bitangent) < 0.0f ? -1.0f : 1.0f;
  return {unit, w};
}

// Appends the vertices and triangles of `mesh` to `data`.
void AppendMesh(const aiMesh& mesh, MeshData& data) {
  const auto base = static_cast<Index>(data.vertices.size());
  const aiVector3D* uvs = mesh.mTextureCoords[0];
  const aiVector3D* uvs1 = mesh.mTextureCoords[1];
  for (unsigned i = 0; i < mesh.mNumVertices; ++i) {
    glm::vec3 normal = ToVec3(mesh.mNormals[i]);
    const float length = glm::length(normal);
    normal = std::isfinite(length) && length > 0.0f ? normal / length
                                                    : glm::vec3(0, 1, 0);
    const glm::vec2 uv =
        uvs != nullptr ? glm::vec2(uvs[i].x, 1.0f - uvs[i].y) : glm::vec2(0);
    const glm::vec2 uv1 =
        uvs1 != nullptr ? glm::vec2(uvs1[i].x, 1.0f - uvs1[i].y) : uv;
    data.vertices.push_back({.position = ToVec3(mesh.mVertices[i]),
                             .normal = normal,
                             .uv0 = uv,
                             .uv1 = uv1,
                             .tangent = VertexTangent(mesh, i, normal)});
  }
  for (const aiFace& face : std::span(mesh.mFaces, mesh.mNumFaces)) {
    for (unsigned i = 0; i < 3; ++i) {
      data.indices.push_back(base + face.mIndices[i]);
    }
  }
}

}  // namespace

bool IsTriangleMesh(const aiMesh& mesh) {
  const unsigned primitive_types =
      mesh.mPrimitiveTypes & ~aiPrimitiveType_NGONEncodingFlag;
  return primitive_types == aiPrimitiveType_TRIANGLE && mesh.mNumFaces != 0;
}

std::vector<const aiMesh*> UsableMeshes(
    std::span<const aiMesh* const> candidates, std::string_view log_name) {
  std::vector<const aiMesh*> meshes;
  for (const aiMesh* mesh : candidates) {
    if (!IsTriangleMesh(*mesh)) {
      continue;
    }
    if (mesh->mNormals == nullptr) {
      core::log::Warn("{}: skipping mesh '{}' without normals", log_name,
                      mesh->mName.C_Str());
      continue;
    }
    meshes.push_back(mesh);
  }
  return meshes;
}

size_t VertexCount(std::span<const aiMesh* const> meshes) {
  size_t count = 0;
  for (const aiMesh* mesh : meshes) {
    count += mesh->mNumVertices;
  }
  return count;
}

MeshData BuildMesh(std::span<const aiMesh* const> meshes,
                   MaterialTable& materials) {
  std::vector<std::pair<int, const aiMesh*>> sorted;
  sorted.reserve(meshes.size());
  size_t index_count = 0;
  for (const aiMesh* mesh : meshes) {
    sorted.emplace_back(materials.Resolve(mesh->mMaterialIndex), mesh);
    index_count += size_t{mesh->mNumFaces} * 3;
  }
  std::ranges::stable_sort(sorted, {}, &std::pair<int, const aiMesh*>::first);

  MeshData data;
  data.vertices.reserve(VertexCount(meshes));
  data.indices.reserve(index_count);
  for (const auto& [material, mesh] : sorted) {
    const auto first = static_cast<uint32_t>(data.indices.size());
    AppendMesh(*mesh, data);
    const auto count = static_cast<uint32_t>(data.indices.size()) - first;
    if (!data.submeshes.empty() && data.submeshes.back().material == material) {
      data.submeshes.back().index_count += count;
    } else {
      data.submeshes.push_back(
          {.first_index = first, .index_count = count, .material = material});
    }
  }
  data.bounds = ComputeBounds(data.vertices);
  return data;
}

}  // namespace blue::asset::internal
