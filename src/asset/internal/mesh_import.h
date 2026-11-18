#ifndef BLUE_ASSET_INTERNAL_MESH_IMPORT_H_
#define BLUE_ASSET_INTERNAL_MESH_IMPORT_H_

#include <assimp/mesh.h>
#include <assimp/postprocess.h>

#include <span>
#include <string_view>
#include <vector>

#include "asset/internal/material_import.h"
#include "asset/mesh_data.h"

// Vertex and submesh conversion shared by the importers in blue_asset.
// Internal to the library: assimp types appear here.

namespace blue::asset::internal {

// Post-processing every importer needs: triangles, smooth normals where
// missing, generated tangents, shared vertices, only triangle meshes.
inline constexpr unsigned kBasePostProcess =
    aiProcess_Triangulate | aiProcess_GenSmoothNormals |
    aiProcess_CalcTangentSpace | aiProcess_JoinIdenticalVertices |
    aiProcess_SortByPType | aiProcess_ImproveCacheLocality |
    aiProcess_ValidateDataStructure;

// Returns true if `mesh` holds only triangles and at least one face.
// Triangulating quads and n-gons only tags the mesh with the n-gon encoding
// flag; its faces are still triangles.
[[nodiscard]] bool IsTriangleMesh(const aiMesh& mesh);

// Returns the meshes of `candidates` that hold triangles and normals,
// warning (prefixed with `log_name`) about meshes skipped for lacking
// normals.
[[nodiscard]] std::vector<const aiMesh*> UsableMeshes(
    std::span<const aiMesh* const> candidates, std::string_view log_name);

// Returns the number of vertices of `meshes` together.
[[nodiscard]] size_t VertexCount(std::span<const aiMesh* const> meshes);

// Builds `meshes` into one MeshData: vertices with UV channel 0 flipped to
// the top-left convention (assimp's glTF and OBJ importers both deliver
// v = 0 at the bottom) and tangents orthonormalized against the normal, with
// the handedness taken against the flipped v; meshes without UVs get zero
// UVs and a deterministic tangent. Meshes are grouped by
// `materials.Resolve()` so each material is one contiguous submesh. The
// caller ensures the vertex count fits Index and `meshes` is not empty.
[[nodiscard]] MeshData BuildMesh(std::span<const aiMesh* const> meshes,
                                 MaterialTable& materials);

}  // namespace blue::asset::internal

#endif  // BLUE_ASSET_INTERNAL_MESH_IMPORT_H_
