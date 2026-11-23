#include "asset/texture_loader.h"

#include <assimp/scene.h>
#include <assimp/texture.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <assimp/Importer.hpp>

#include "asset/internal/embedded_textures.h"
#include "core/log.h"

namespace blue::asset {
namespace {

using ImageResult = core::Result<ImageData, ImageError>;

// Returns the RGBA8 image of assimp's uncompressed texels (b, g, r, a).
ImageData FromTexels(const aiTexture& texture) {
  ImageData image{.width = static_cast<int>(texture.mWidth),
                  .height = static_cast<int>(texture.mHeight)};
  const std::span<const aiTexel> texels(
      texture.pcData, size_t{texture.mWidth} * texture.mHeight);
  image.pixels.reserve(texels.size() * 4);
  for (const aiTexel& texel : texels) {
    image.pixels.insert(image.pixels.end(),
                        {texel.r, texel.g, texel.b, texel.a});
  }
  return image;
}

// Returns the encoded file bytes of a compressed embedded texture.
std::span<const std::byte> CompressedBytes(const aiTexture& texture) {
  // Compressed: mWidth is the byte size of the file data in pcData.
  return std::as_bytes(
      std::span(reinterpret_cast<const char*>(texture.pcData), texture.mWidth));
}

}  // namespace

namespace internal {

std::vector<std::unique_ptr<aiTexture>> TakeEmbeddedTextures(aiScene& scene) {
  std::vector<std::unique_ptr<aiTexture>> textures;
  textures.reserve(scene.mNumTextures);
  for (unsigned i = 0; i < scene.mNumTextures; ++i) {
    textures.emplace_back(scene.mTextures[i]);
    scene.mTextures[i] = nullptr;  // ~aiScene deletes the remaining ones
  }
  return textures;
}

}  // namespace internal

EmbeddedImages::EmbeddedImages() = default;

EmbeddedImages::EmbeddedImages(std::vector<std::unique_ptr<aiTexture>> textures)
    : textures_(std::move(textures)) {}

EmbeddedImages::EmbeddedImages(EmbeddedImages&&) noexcept = default;
EmbeddedImages& EmbeddedImages::operator=(EmbeddedImages&&) noexcept = default;
EmbeddedImages::~EmbeddedImages() = default;

int EmbeddedImages::count() const { return static_cast<int>(textures_.size()); }

size_t EmbeddedImages::byte_size() const {
  size_t total = 0;
  for (const std::unique_ptr<aiTexture>& texture : textures_) {
    if (texture == nullptr) {
      continue;
    }
    total += texture->mHeight == 0
                 ? size_t{texture->mWidth}
                 : size_t{texture->mWidth} * texture->mHeight * sizeof(aiTexel);
  }
  return total;
}

core::Result<ImageSize, ImageError> EmbeddedImages::Probe(int index) const {
  using Result = core::Result<ImageSize, ImageError>;
  if (index < 0 || index >= count() ||
      textures_[static_cast<size_t>(index)] == nullptr) {
    return Result::Error(ImageError::kNoTexture);
  }
  const aiTexture& texture = *textures_[static_cast<size_t>(index)];
  if (texture.mHeight == 0) {
    return ProbeImage(CompressedBytes(texture));
  }
  if (texture.mWidth > static_cast<unsigned>(kMaxImageDimension) ||
      texture.mHeight > static_cast<unsigned>(kMaxImageDimension)) {
    return Result::Error(ImageError::kTooLarge);
  }
  return Result::Ok({.width = static_cast<int>(texture.mWidth),
                     .height = static_cast<int>(texture.mHeight)});
}

core::Result<ImageData, ImageError> EmbeddedImages::Decode(int index) const {
  if (index < 0 || index >= count() ||
      textures_[static_cast<size_t>(index)] == nullptr) {
    return ImageResult::Error(ImageError::kNoTexture);
  }
  const aiTexture& texture = *textures_[static_cast<size_t>(index)];
  if (texture.mHeight == 0) {
    return DecodeImage(CompressedBytes(texture));
  }
  if (texture.mWidth > static_cast<unsigned>(kMaxImageDimension) ||
      texture.mHeight > static_cast<unsigned>(kMaxImageDimension)) {
    return ImageResult::Error(ImageError::kTooLarge);
  }
  return ImageResult::Ok(FromTexels(texture));
}

void EmbeddedImages::Release(int index) {
  if (index >= 0 && index < count()) {
    textures_[static_cast<size_t>(index)].reset();
  }
}

core::Result<EmbeddedImages, ImageError> LoadEmbeddedImages(
    const std::string& path) {
  using Result = core::Result<EmbeddedImages, ImageError>;
  Assimp::Importer importer;
  if (importer.ReadFile(path.c_str(), 0) == nullptr) {
    core::log::Warn("cannot open '{}' for its textures: {}", path,
                    importer.GetErrorString());
    return Result::Error(ImageError::kFileNotFound);
  }
  const std::unique_ptr<aiScene> scene(importer.GetOrphanedScene());
  return Result::Ok(EmbeddedImages(internal::TakeEmbeddedTextures(*scene)));
}

core::Result<ImageData, ImageError> LoadTexture(
    const material::TextureRef& ref) {
  return std::move(LoadTextures(std::span(&ref, 1)).front());
}

std::vector<core::Result<ImageData, ImageError>> LoadTextures(
    std::span<const material::TextureRef> refs) {
  std::vector<ImageResult> results;
  results.reserve(refs.size());
  // The embedded images of each model file, loaded on first use and kept for
  // the rest of the batch; an error remembers a model that failed to open.
  std::map<std::string, core::Result<EmbeddedImages, ImageError>, std::less<>>
      models;
  for (const material::TextureRef& ref : refs) {
    if (ref.empty()) {
      results.push_back(ImageResult::Error(ImageError::kNoTexture));
      continue;
    }
    if (!ref.embedded()) {
      results.push_back(LoadImageFile(ref.path));
      continue;
    }
    auto found = models.find(ref.path);
    if (found == models.end()) {
      found = models.emplace(ref.path, LoadEmbeddedImages(ref.path)).first;
    }
    if (!found->second.ok()) {
      results.push_back(ImageResult::Error(found->second.error()));
      continue;
    }
    results.push_back(found->second.value().Decode(ref.embedded_index));
  }
  return results;
}

}  // namespace blue::asset
