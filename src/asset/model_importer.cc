#include "asset/model_importer.h"

#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>
#include <glm/common.hpp>

#include "asset/internal/material_color.h"
#include "core/log.h"
#include "core/path.h"
#include "platform/file_system.h"

namespace blue::asset {
namespace {

constexpr unsigned kPostProcess =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals |
    aiProcess_JoinIdenticalVertices | aiProcess_PreTransformVertices |
    aiProcess_SortByPType | aiProcess_ImproveCacheLocality |
    aiProcess_ValidateDataStructure;

}  // namespace

const char* ToString(ImportError error) {
  switch (error) {
    case ImportError::kFileNotFound:
      return "file not found";
    case ImportError::kParseFailed:
      return "parse failed";
    case ImportError::kNoMeshes:
      return "no triangle meshes";
    case ImportError::kTooLarge:
      return "too many vertices";
  }
  return "unknown";
}

core::Result<MeshData, ImportError> ImportModel(const std::string& path,
                                                const ImportOptions& options) {
  using Result = core::Result<MeshData, ImportError>;
  if (!platform::IsRegularFile(path)) {
    return Result::Error(ImportError::kFileNotFound);
  }

  const auto start = std::chrono::steady_clock::now();

  Assimp::Importer importer;
  importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                              aiPrimitiveType_POINT | aiPrimitiveType_LINE);

  const aiScene* scene = importer.ReadFile(path.c_str(), kPostProcess);
  if (scene == nullptr || scene->mRootNode == nullptr) {
    core::log::Error("ImportModel: {}: {}", path, importer.GetErrorString());
    return Result::Error(ImportError::kParseFailed);
  }

  std::vector<const aiMesh*> meshes;
  size_t vertex_count = 0;
  size_t index_count = 0;
  for (const aiMesh* mesh : std::span(scene->mMeshes, scene->mNumMeshes)) {
    if (!internal::IsTriangleMesh(*mesh)) {
      continue;
    }
    if (mesh->mNormals == nullptr) {
      core::log::Warn("ImportModel: {}: skipping mesh '{}' without normals",
                      path, mesh->mName.C_Str());
      continue;
    }
    meshes.push_back(mesh);
    vertex_count += mesh->mNumVertices;
    index_count += size_t{mesh->mNumFaces} * 3;
  }
  if (meshes.empty()) {
    return Result::Error(ImportError::kNoMeshes);
  }
  if (vertex_count > std::numeric_limits<Index>::max()) {
    return Result::Error(ImportError::kTooLarge);
  }

  MeshData data;
  data.vertices.reserve(vertex_count);
  data.indices.reserve(index_count);

  for (const aiMesh* mesh : meshes) {
    const glm::vec3 color =
        internal::MeshColor(*scene, *mesh, options.default_color);
    const auto base = static_cast<Index>(data.vertices.size());

    for (unsigned i = 0; i < mesh->mNumVertices; ++i) {
      const aiVector3D& p = mesh->mVertices[i];
      const aiVector3D& n = mesh->mNormals[i];
      data.vertices.push_back({{p.x, p.y, p.z}, {n.x, n.y, n.z}, color});
    }
    for (const aiFace& face : std::span(mesh->mFaces, mesh->mNumFaces)) {
      for (unsigned i = 0; i < 3; ++i) {
        data.indices.push_back(base + face.mIndices[i]);
      }
    }
  }

  data.bounds = {.min = data.vertices.front().position,
                 .max = data.vertices.front().position};
  for (const Vertex& vertex : data.vertices) {
    data.bounds.min = glm::min(data.bounds.min, vertex.position);
    data.bounds.max = glm::max(data.bounds.max, vertex.position);
  }

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  core::log::Info(
      "imported {}: {} vertices, {} triangles, {} meshes in {:.1f} ms",
      core::FileName(path), data.vertices.size(), data.indices.size() / 3,
      meshes.size(), elapsed.count());
  return Result::Ok(std::move(data));
}

}  // namespace blue::asset
