#include "asset/texture_loader.h"

#include <assimp/scene.h>
#include <assimp/texture.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <assimp/Importer.hpp>

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

// Decodes embedded texture `index` of `scene`.
ImageResult DecodeEmbedded(const aiScene& scene, int index) {
  if (index < 0 || static_cast<unsigned>(index) >= scene.mNumTextures) {
    return ImageResult::Error(ImageError::kNoTexture);
  }
  const aiTexture& texture = *scene.mTextures[index];
  if (texture.mHeight != 0) {
    if (texture.mWidth > static_cast<unsigned>(kMaxImageDimension) ||
        texture.mHeight > static_cast<unsigned>(kMaxImageDimension)) {
      return ImageResult::Error(ImageError::kTooLarge);
    }
    return ImageResult::Ok(FromTexels(texture));
  }
  // Compressed: mWidth is the byte size of the file data in pcData.
  return DecodeImage(std::as_bytes(std::span(
      reinterpret_cast<const char*>(texture.pcData), texture.mWidth)));
}

// Opens `path` with assimp just far enough to reach its embedded textures.
std::unique_ptr<Assimp::Importer> OpenModel(const std::string& path) {
  auto importer = std::make_unique<Assimp::Importer>();
  const aiScene* scene = importer->ReadFile(path.c_str(), 0);
  if (scene == nullptr) {
    core::log::Warn("cannot open '{}' for its textures: {}", path,
                    importer->GetErrorString());
    return nullptr;
  }
  return importer;
}

}  // namespace

core::Result<ImageData, ImageError> LoadTexture(
    const material::TextureRef& ref) {
  return std::move(LoadTextures(std::span(&ref, 1)).front());
}

std::vector<core::Result<ImageData, ImageError>> LoadTextures(
    std::span<const material::TextureRef> refs) {
  std::vector<ImageResult> results;
  results.reserve(refs.size());
  // One importer per model file, opened on first use and kept for the rest
  // of the batch; nullptr remembers a model that failed to open.
  std::map<std::string, std::unique_ptr<Assimp::Importer>, std::less<>> models;
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
      found = models.emplace(ref.path, OpenModel(ref.path)).first;
    }
    if (found->second == nullptr) {
      results.push_back(ImageResult::Error(ImageError::kFileNotFound));
      continue;
    }
    results.push_back(
        DecodeEmbedded(*found->second->GetScene(), ref.embedded_index));
  }
  return results;
}

}  // namespace blue::asset
