#ifndef BLUE_ASSET_INTERNAL_MATERIAL_COLOR_H_
#define BLUE_ASSET_INTERNAL_MATERIAL_COLOR_H_

#include <assimp/mesh.h>
#include <assimp/scene.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

// Material colour helpers shared by the importers in blue_asset. Internal to
// the library: assimp types appear here, so only blue_asset sources include it.

namespace blue::asset::internal {

// Returns true if `mesh` holds only triangles and at least one face.
// Triangulating quads and n-gons only tags the mesh with the n-gon encoding
// flag; its faces are still triangles.
[[nodiscard]] bool IsTriangleMesh(const aiMesh& mesh);

// Returns the diffuse (glTF: base colour factor) colour of the material of
// `mesh`, or `fallback` when it has none. Assimp's fallback material is not
// authored content, so it must not override the caller's colour. Material
// colours are used as-is, i.e. treated as linear.
[[nodiscard]] glm::vec3 MeshColor(const aiScene& scene, const aiMesh& mesh,
                                  const glm::vec3& fallback);

// An 8-bit RGBA image, rows top to bottom.
struct DecodedImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4 bytes
};

// Where a material's base-colour texture comes from.
struct BaseColorTexture {
  const aiTexture* texture = nullptr;  // embedded image, never null if set
  unsigned uv_channel = 0;             // index into aiMesh::mTextureCoords
};

// Returns the embedded base-colour (or, failing that, diffuse) texture of
// `material`, or nullopt if it has none or references an external file.
[[nodiscard]] std::optional<BaseColorTexture> FindBaseColorTexture(
    const aiScene& scene, const aiMaterial& material);

// Decodes `texture`: compressed PNG/BMP data through SDL, or assimp's raw
// BGRA texels. Returns nullopt and logs a warning if decoding fails.
[[nodiscard]] std::optional<DecodedImage> DecodeTexture(
    const aiTexture& texture);

// Maps 8-bit sRGB-encoded values to linear [0, 1].
using SrgbTable = std::array<float, 256>;

// Returns the sRGB decoding table (IEC 61966-2-1 transfer function).
[[nodiscard]] SrgbTable MakeSrgbTable();

// Returns the linear colour of `image` at `uv` with bilinear filtering and
// repeat wrapping. `uv` uses assimp's convention (v = 0 at the bottom of the
// image, which assimp's glTF importer produces by flipping glTF's v).
// `image` must not be empty.
[[nodiscard]] glm::vec3 SampleLinear(const DecodedImage& image,
                                     const SrgbTable& srgb, glm::vec2 uv);

}  // namespace blue::asset::internal

#endif  // BLUE_ASSET_INTERNAL_MATERIAL_COLOR_H_
