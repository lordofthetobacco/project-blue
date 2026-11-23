#ifndef BLUE_ASSET_TEXTURE_LOADER_H_
#define BLUE_ASSET_TEXTURE_LOADER_H_

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "asset/image_decoder.h"
#include "core/result.h"
#include "material/material.h"

// Loading of material::TextureRef images, including images embedded in model
// files (through assimp). Part of blue_asset; image_decoder.h itself needs no
// importer.

struct aiTexture;

namespace blue::asset {

// The images embedded in one model file, still encoded (PNG/JPEG bytes, or
// assimp's raw BGRA texels), taken over from an import so that they can be
// decoded one at a time, and freed one at a time, without parsing the file
// again. Index i is TextureRef::embedded_index i of that file. Move-only.
// Decode() and size queries may run concurrently with each other and with
// Release() of a different index.
class EmbeddedImages {
 public:
  EmbeddedImages();
  // Takes `textures` over (the importer's aiScene::mTextures, detached from
  // their scene); null entries count as already released.
  explicit EmbeddedImages(std::vector<std::unique_ptr<aiTexture>> textures);
  EmbeddedImages(const EmbeddedImages&) = delete;
  EmbeddedImages& operator=(const EmbeddedImages&) = delete;
  EmbeddedImages(EmbeddedImages&&) noexcept;
  EmbeddedImages& operator=(EmbeddedImages&&) noexcept;
  ~EmbeddedImages();

  // Returns the number of images the file embeds (released ones included).
  [[nodiscard]] int count() const;

  // Returns the bytes of the encoded images still held.
  [[nodiscard]] size_t byte_size() const;

  // Returns the width and height image `index` decodes to, read from its
  // header only; kNoTexture for an index out of range or released,
  // kDecodeFailed if the header is not readable.
  [[nodiscard]] core::Result<ImageSize, ImageError> Probe(int index) const;

  // Decodes image `index` to RGBA8. kNoTexture for an index out of range or
  // released; otherwise the errors of DecodeImage().
  [[nodiscard]] core::Result<ImageData, ImageError> Decode(int index) const;

  // Frees the encoded bytes of image `index`; later calls for it fail with
  // kNoTexture. Out-of-range indices are ignored.
  void Release(int index);

 private:
  std::vector<std::unique_ptr<aiTexture>> textures_;
};

// Parses the model file at `path` without post-processing and keeps only
// its embedded images. Fails with kFileNotFound if the file cannot be read
// or parsed. Prefer ImportAnimatedModel()'s `embedded` output when the model
// is imported anyway: that avoids a second parse.
[[nodiscard]] core::Result<EmbeddedImages, ImageError> LoadEmbeddedImages(
    const std::string& path);

// Loads the image `ref` names: an image file, or an embedded texture of a
// model file (compressed PNG/JPEG data or assimp's raw BGRA texels). An
// embedded load parses the whole model file again: prefer LoadTextures()
// for several textures of one model.
[[nodiscard]] core::Result<ImageData, ImageError> LoadTexture(
    const material::TextureRef& ref);

// Loads every texture in `refs`, parsing each referenced model file only
// once. result[i] belongs to refs[i]. Every image is decoded and held at
// once: for large models decode them one by one from EmbeddedImages.
[[nodiscard]] std::vector<core::Result<ImageData, ImageError>> LoadTextures(
    std::span<const material::TextureRef> refs);

}  // namespace blue::asset

#endif  // BLUE_ASSET_TEXTURE_LOADER_H_
