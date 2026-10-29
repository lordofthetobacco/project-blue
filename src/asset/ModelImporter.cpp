#include "asset/ModelImporter.hpp"

#include "core/Log.hpp"
#include "core/Path.hpp"

#include <assimp/Importer.hpp>
#include <assimp/material.h>
#include <assimp/mesh.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <glm/common.hpp>

#include <chrono>
#include <cstddef>
#include <limits>
#include <span>
#include <system_error>
#include <vector>

namespace blue {

namespace {

constexpr unsigned kPostProcess = aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                                  aiProcess_JoinIdenticalVertices | aiProcess_PreTransformVertices |
                                  aiProcess_SortByPType | aiProcess_ImproveCacheLocality |
                                  aiProcess_ValidateDataStructure;

// Assimp's fallback material is not authored content, so it must not override the caller's colour.
// Material colours are used as-is, i.e. treated as linear.
glm::vec3 meshColor(const aiScene& scene, const aiMesh& mesh, const glm::vec3& fallback)
{
    if (mesh.mMaterialIndex >= scene.mNumMaterials) {
        return fallback;
    }
    const aiMaterial& material = *scene.mMaterials[mesh.mMaterialIndex];

    aiString name;
    if (material.Get(AI_MATKEY_NAME, name) == aiReturn_SUCCESS &&
        name == aiString(AI_DEFAULT_MATERIAL_NAME)) {
        return fallback;
    }

    aiColor3D diffuse;
    if (material.Get(AI_MATKEY_COLOR_DIFFUSE, diffuse) != aiReturn_SUCCESS) {
        return fallback;
    }
    return {diffuse.r, diffuse.g, diffuse.b};
}

} // namespace

const char* toString(ImportError error)
{
    switch (error) {
    case ImportError::FileNotFound: return "file not found";
    case ImportError::ParseFailed: return "parse failed";
    case ImportError::NoMeshes: return "no triangle meshes";
    case ImportError::TooLarge: return "too many vertices";
    }
    return "unknown";
}

std::expected<MeshData, ImportError> importModel(const std::filesystem::path& path, const ImportOptions& options)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::unexpected(ImportError::FileNotFound);
    }

    const auto start = std::chrono::steady_clock::now();

    Assimp::Importer importer;
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    const std::string utf8Path = toUtf8(path);
    const aiScene* scene = importer.ReadFile(utf8Path.c_str(), kPostProcess);
    if (scene == nullptr || scene->mRootNode == nullptr) {
        log::error("importModel: {}: {}", utf8Path, importer.GetErrorString());
        return std::unexpected(ImportError::ParseFailed);
    }

    std::vector<const aiMesh*> meshes;
    size_t vertexCount = 0;
    size_t indexCount = 0;
    for (const aiMesh* mesh : std::span(scene->mMeshes, scene->mNumMeshes)) {
        if (mesh->mPrimitiveTypes != aiPrimitiveType_TRIANGLE || mesh->mNumFaces == 0) {
            continue;
        }
        if (mesh->mNormals == nullptr) {
            log::warn("importModel: {}: skipping mesh '{}' without normals", utf8Path, mesh->mName.C_Str());
            continue;
        }
        meshes.push_back(mesh);
        vertexCount += mesh->mNumVertices;
        indexCount += size_t{mesh->mNumFaces} * 3;
    }
    if (meshes.empty()) {
        return std::unexpected(ImportError::NoMeshes);
    }
    if (vertexCount > std::numeric_limits<Index>::max()) {
        return std::unexpected(ImportError::TooLarge);
    }

    MeshData data;
    data.vertices.reserve(vertexCount);
    data.indices.reserve(indexCount);

    for (const aiMesh* mesh : meshes) {
        const glm::vec3 color = meshColor(*scene, *mesh, options.defaultColor);
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

    data.bounds = {.min = data.vertices.front().position, .max = data.vertices.front().position};
    for (const Vertex& vertex : data.vertices) {
        data.bounds.min = glm::min(data.bounds.min, vertex.position);
        data.bounds.max = glm::max(data.bounds.max, vertex.position);
    }

    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start);
    log::info("imported {}: {} vertices, {} triangles, {} meshes in {:.1f} ms", toUtf8(path.filename()),
              data.vertices.size(), data.indices.size() / 3, meshes.size(), elapsed.count());
    return data;
}

} // namespace blue
