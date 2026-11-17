#include "asset/internal/material_color.h"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <assimp/material.h>
#include <assimp/texture.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <span>

#include "core/log.h"

namespace blue::asset::internal {
namespace {

// Returns the image of an SDL surface converted to RGBA8, or nullopt.
std::optional<DecodedImage> FromSurface(SDL_Surface* surface) {
  SDL_Surface* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA32);
  SDL_DestroySurface(surface);
  if (rgba == nullptr) {
    return std::nullopt;
  }
  DecodedImage image{.width = rgba->w, .height = rgba->h};
  const auto row_bytes = static_cast<size_t>(rgba->w) * 4;
  image.rgba.resize(row_bytes * static_cast<size_t>(rgba->h));
  const auto* source = static_cast<const uint8_t*>(rgba->pixels);
  for (int y = 0; y < rgba->h; ++y) {
    std::memcpy(image.rgba.data() + row_bytes * static_cast<size_t>(y),
                source + static_cast<ptrdiff_t>(rgba->pitch) * y, row_bytes);
  }
  SDL_DestroySurface(rgba);
  return image;
}

// Returns the texel at (x, y), wrapping both coordinates, as linear RGB.
glm::vec3 Texel(const DecodedImage& image, const SrgbTable& srgb, int x,
                int y) {
  x %= image.width;
  y %= image.height;
  x += x < 0 ? image.width : 0;
  y += y < 0 ? image.height : 0;
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
       static_cast<size_t>(x)) *
      4;
  return {srgb[image.rgba[offset]], srgb[image.rgba[offset + 1]],
          srgb[image.rgba[offset + 2]]};
}

}  // namespace

bool IsTriangleMesh(const aiMesh& mesh) {
  const unsigned primitive_types =
      mesh.mPrimitiveTypes & ~aiPrimitiveType_NGONEncodingFlag;
  return primitive_types == aiPrimitiveType_TRIANGLE && mesh.mNumFaces != 0;
}

glm::vec3 MeshColor(const aiScene& scene, const aiMesh& mesh,
                    const glm::vec3& fallback) {
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

std::optional<BaseColorTexture> FindBaseColorTexture(
    const aiScene& scene, const aiMaterial& material) {
  for (const aiTextureType type :
       {aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE}) {
    aiString path;
    unsigned uv_channel = 0;
    if (material.GetTexture(type, 0, &path, nullptr, &uv_channel) !=
        aiReturn_SUCCESS) {
      continue;
    }
    const aiTexture* texture = scene.GetEmbeddedTexture(path.C_Str());
    if (texture == nullptr) {
      return std::nullopt;
    }
    return BaseColorTexture{.texture = texture, .uv_channel = uv_channel};
  }
  return std::nullopt;
}

std::optional<DecodedImage> DecodeTexture(const aiTexture& texture) {
  if (texture.mHeight != 0) {
    // Uncompressed: mWidth * mHeight aiTexel (b, g, r, a).
    DecodedImage image{.width = static_cast<int>(texture.mWidth),
                       .height = static_cast<int>(texture.mHeight)};
    const std::span<const aiTexel> texels(
        texture.pcData, size_t{texture.mWidth} * texture.mHeight);
    image.rgba.reserve(texels.size() * 4);
    for (const aiTexel& texel : texels) {
      image.rgba.insert(image.rgba.end(), {texel.r, texel.g, texel.b, texel.a});
    }
    return image;
  }
  // Compressed: mWidth is the byte size of the file data in pcData.
  SDL_IOStream* stream = SDL_IOFromConstMem(texture.pcData, texture.mWidth);
  SDL_Surface* surface =
      stream == nullptr ? nullptr : SDL_LoadSurface_IO(stream, true);
  std::optional<DecodedImage> image =
      surface == nullptr ? std::nullopt : FromSurface(surface);
  if (!image.has_value()) {
    core::log::Warn("cannot decode texture '{}' ({}): {}",
                    texture.mFilename.C_Str(), texture.achFormatHint,
                    SDL_GetError());
  }
  return image;
}

SrgbTable MakeSrgbTable() {
  SrgbTable table{};
  for (size_t i = 0; i < table.size(); ++i) {
    const float encoded = static_cast<float>(i) / 255.0f;
    table[i] = encoded <= 0.04045f
                   ? encoded / 12.92f
                   : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
  }
  return table;
}

glm::vec3 SampleLinear(const DecodedImage& image, const SrgbTable& srgb,
                       glm::vec2 uv) {
  // Texel centres sit at half-integer coordinates; row 0 is the top (v = 1).
  const float x = uv.x * static_cast<float>(image.width) - 0.5f;
  const float y = (1.0f - uv.y) * static_cast<float>(image.height) - 0.5f;
  const float x_floor = std::floor(x);
  const float y_floor = std::floor(y);
  const float fx = x - x_floor;
  const float fy = y - y_floor;
  // Wrap before converting so large UVs cannot overflow int.
  const auto x0 =
      static_cast<int>(std::fmod(x_floor, static_cast<float>(image.width)));
  const auto y0 =
      static_cast<int>(std::fmod(y_floor, static_cast<float>(image.height)));
  const glm::vec3 top = Texel(image, srgb, x0, y0) * (1.0f - fx) +
                        Texel(image, srgb, x0 + 1, y0) * fx;
  const glm::vec3 bottom = Texel(image, srgb, x0, y0 + 1) * (1.0f - fx) +
                           Texel(image, srgb, x0 + 1, y0 + 1) * fx;
  return top * (1.0f - fy) + bottom * fy;
}

}  // namespace blue::asset::internal
