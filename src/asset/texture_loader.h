#ifndef BLUE_ASSET_TEXTURE_LOADER_H_
#define BLUE_ASSET_TEXTURE_LOADER_H_

#include <span>
#include <vector>

#include "asset/image_decoder.h"
#include "core/result.h"
#include "material/material.h"

// Loading of material::TextureRef images, including images embedded in model
// files (through assimp). Part of blue_asset; image_decoder.h itself needs no
// importer.

namespace blue::asset {

// Loads the image `ref` names: an image file, or an embedded texture of a
// model file (compressed PNG/JPEG data or assimp's raw BGRA texels). An
// embedded load parses the whole model file again: prefer LoadTextures()
// for several textures of one model.
[[nodiscard]] core::Result<ImageData, ImageError> LoadTexture(
    const material::TextureRef& ref);

// Loads every texture in `refs`, parsing each referenced model file only
// once. result[i] belongs to refs[i].
[[nodiscard]] std::vector<core::Result<ImageData, ImageError>> LoadTextures(
    std::span<const material::TextureRef> refs);

}  // namespace blue::asset

#endif  // BLUE_ASSET_TEXTURE_LOADER_H_
