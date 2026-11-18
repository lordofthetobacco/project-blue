#include "asset/model_importer.h"

#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <chrono>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>

#include "asset/internal/material_import.h"
#include "asset/internal/mesh_import.h"
#include "core/log.h"
#include "core/path.h"
#include "platform/file_system.h"

namespace blue::asset {
namespace {

constexpr unsigned kPostProcess =
    internal::kBasePostProcess | aiProcess_PreTransformVertices;

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

core::Result<Model, ImportError> ImportModel(const std::string& path,
                                             const ImportOptions& options) {
  using Result = core::Result<Model, ImportError>;
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

  const std::vector<const aiMesh*> meshes = internal::UsableMeshes(
      std::span(scene->mMeshes, scene->mNumMeshes), "ImportModel: " + path);
  if (meshes.empty()) {
    return Result::Error(ImportError::kNoMeshes);
  }
  if (internal::VertexCount(meshes) > std::numeric_limits<Index>::max()) {
    return Result::Error(ImportError::kTooLarge);
  }

  internal::MaterialTable materials(*scene, path,
                                    options.use_material_overrides);
  Model model{.mesh = internal::BuildMesh(meshes, materials)};
  model.materials = materials.Release();

  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  core::log::Info(
      "imported {}: {} vertices, {} triangles, {} meshes, {} materials in "
      "{:.1f} ms",
      core::FileName(path), model.mesh.vertices.size(),
      model.mesh.indices.size() / 3, meshes.size(), model.materials.size(),
      elapsed.count());
  return Result::Ok(std::move(model));
}

}  // namespace blue::asset
