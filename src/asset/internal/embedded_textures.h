#ifndef BLUE_ASSET_INTERNAL_EMBEDDED_TEXTURES_H_
#define BLUE_ASSET_INTERNAL_EMBEDDED_TEXTURES_H_

#include <assimp/scene.h>
#include <assimp/texture.h>

#include <memory>
#include <vector>

// Hand-over of a scene's embedded textures to asset::EmbeddedImages, shared
// by the importers in blue_asset. Internal to the library: assimp types
// appear here.

namespace blue::asset::internal {

// Detaches every embedded texture from `scene` (its entries become null, so
// deleting the scene keeps them) and returns them; index i is embedded
// texture i. `scene` must be owned by the caller (an orphaned scene).
[[nodiscard]] std::vector<std::unique_ptr<aiTexture>> TakeEmbeddedTextures(
    aiScene& scene);

}  // namespace blue::asset::internal

#endif  // BLUE_ASSET_INTERNAL_EMBEDDED_TEXTURES_H_
