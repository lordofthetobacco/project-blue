#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "asset/image_decoder.h"
#include "cook/texture_cook.h"
#include "prefab/prefab.h"

namespace blue::cook {
namespace {

using asset::ImageData;
using prefab::TextureFormat;

ImageData Solid(int width, int height, uint8_t r, uint8_t g, uint8_t b,
                uint8_t a = 255) {
  ImageData image{.width = width, .height = height};
  for (int i = 0; i < width * height; ++i) {
    image.pixels.insert(image.pixels.end(), {r, g, b, a});
  }
  return image;
}

// A smooth colour field with some detail: gradients plus a soft ripple,
// typical of real textures and representative for block compression.
ImageData Gradient(int width, int height) {
  ImageData image{.width = width, .height = height};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float u = static_cast<float>(x) / width;
      const float v = static_cast<float>(y) / height;
      const float ripple = 0.5f + 0.5f * std::sin(u * 6.0f + v * 4.0f);
      image.pixels.insert(
          image.pixels.end(),
          {static_cast<uint8_t>(255.0f * u), static_cast<uint8_t>(255.0f * v),
           static_cast<uint8_t>(255.0f * ripple), 255});
    }
  }
  return image;
}

// A bumpy tangent-space normal map: unit normals encoded as n * 0.5 + 0.5.
ImageData NormalMap(int width, int height) {
  ImageData image{.width = width, .height = height};
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const float nx = 0.4f * std::sin(static_cast<float>(x) * 0.3f);
      const float ny = 0.4f * std::cos(static_cast<float>(y) * 0.2f);
      const float nz = std::sqrt(1.0f - nx * nx - ny * ny);
      const auto encode = [](float n) {
        return static_cast<uint8_t>((n * 0.5f + 0.5f) * 255.0f + 0.5f);
      };
      image.pixels.insert(image.pixels.end(),
                          {encode(nx), encode(ny), encode(nz), 255});
    }
  }
  return image;
}

struct ErrorStats {
  int max = 0;
  double mean = 0.0;
};

// Returns the per-channel absolute error of `channels` channels between the
// two images (same size).
ErrorStats Compare(const ImageData& a, const ImageData& b, int channels) {
  ErrorStats stats;
  size_t count = 0;
  for (size_t i = 0; i < a.pixels.size(); ++i) {
    if (static_cast<int>(i % 4) >= channels) {
      continue;
    }
    const int error =
        std::abs(static_cast<int>(a.pixels[i]) - static_cast<int>(b.pixels[i]));
    stats.max = std::max(stats.max, error);
    stats.mean += error;
    ++count;
  }
  stats.mean /= static_cast<double>(std::max<size_t>(count, 1));
  return stats;
}

std::vector<std::byte> AllBytes(const prefab::Texture& texture) {
  std::vector<std::byte> bytes;
  for (const prefab::TextureMip& mip : texture.mips) {
    bytes.insert(bytes.end(), mip.data.begin(), mip.data.end());
  }
  return bytes;
}

TEST_CASE("GenerateMipChain halves down to 1x1 with exact sizes") {
  const std::vector<ImageData> chain =
      GenerateMipChain(Solid(13, 7, 10, 20, 30), TextureUsage::kLinear);
  REQUIRE(chain.size() == static_cast<size_t>(prefab::FullMipCount(13, 7)));
  CHECK(chain.size() == 4);
  const int expected[][2] = {{13, 7}, {6, 3}, {3, 1}, {1, 1}};
  for (size_t i = 0; i < chain.size(); ++i) {
    CAPTURE(i);
    CHECK(chain[i].width == expected[i][0]);
    CHECK(chain[i].height == expected[i][1]);
    CHECK(chain[i].pixels.size() ==
          static_cast<size_t>(chain[i].width) * chain[i].height * 4);
    // A constant image stays constant, odd sizes included (weights sum to 1).
    CHECK(chain[i].pixels[0] == 10);
    CHECK(chain[i].pixels[1] == 20);
    CHECK(chain[i].pixels[2] == 30);
    CHECK(chain[i].pixels.back() == 255);
  }
}

TEST_CASE("Colour mips average in linear light, data mips in value") {
  ImageData checker{.width = 2, .height = 2};
  checker.pixels = {0,   0,   0,   255, 255, 255, 255, 255,
                    255, 255, 255, 255, 0,   0,   0,   255};
  const auto color = GenerateMipChain(checker, TextureUsage::kColor);
  const auto data = GenerateMipChain(checker, TextureUsage::kLinear);
  REQUIRE(color.size() == 2);
  REQUIRE(data.size() == 2);
  // Linear 0.5 encodes to sRGB 188; a gamma-naive average would give 128.
  CHECK(color[1].pixels[0] == 188);
  CHECK(data[1].pixels[0] == 128);
  CHECK(color[1].pixels[3] == 255);
}

TEST_CASE("Normal map mips are renormalised") {
  const auto chain = GenerateMipChain(NormalMap(32, 32), TextureUsage::kNormal);
  REQUIRE(chain.size() == 6);
  for (size_t level = 1; level < chain.size(); ++level) {
    const ImageData& image = chain[level];
    for (size_t i = 0; i < image.pixels.size(); i += 4) {
      const auto decode = [&](size_t c) {
        return static_cast<float>(image.pixels[i + c]) / 255.0f * 2.0f - 1.0f;
      };
      const float length =
          std::sqrt(decode(0) * decode(0) + decode(1) * decode(1) +
                    decode(2) * decode(2));
      CHECK(length == doctest::Approx(1.0f).epsilon(0.02));
    }
  }
}

TEST_CASE("max_size halves the base level until it fits") {
  const auto chain =
      GenerateMipChain(Solid(100, 40, 1, 2, 3), TextureUsage::kLinear, 30);
  REQUIRE(!chain.empty());
  CHECK(chain[0].width == 25);
  CHECK(chain[0].height == 10);
}

TEST_CASE("BC7 sRGB round trip stays within error bounds") {
  prefab::BulkStorage storage;
  const ImageData image = Gradient(64, 64);
  for (const CookQuality quality : {CookQuality::kFast, CookQuality::kHigh}) {
    CAPTURE(static_cast<int>(quality));
    auto cooked = CookTexture(
        image, {.usage = TextureUsage::kColor, .quality = quality}, storage);
    REQUIRE(cooked.ok());
    const prefab::Texture& texture = cooked.value();
    CHECK(texture.format == TextureFormat::kBc7Srgb);
    CHECK(texture.width == 64);
    CHECK(texture.mip_count() == prefab::FullMipCount(64, 64));
    for (const prefab::TextureMip& mip : texture.mips) {
      CHECK(mip.data.size() ==
            prefab::MipByteSize(texture.format, mip.width, mip.height));
      CHECK(reinterpret_cast<uintptr_t>(mip.data.data()) %
                prefab::kBulkAlignment ==
            0);
    }
    const ErrorStats error = Compare(image, DecodeTextureMip(texture, 0), 4);
    CHECK(error.max <= 16);
    CHECK(error.mean < 2.0);
  }
}

TEST_CASE("BC7 UNORM keeps packed ORM data") {
  prefab::BulkStorage storage;
  const ImageData image = Gradient(32, 32);
  auto cooked = CookTexture(image, {.usage = TextureUsage::kLinear}, storage);
  REQUIRE(cooked.ok());
  CHECK(cooked.value().format == TextureFormat::kBc7Unorm);
  const ErrorStats error =
      Compare(image, DecodeTextureMip(cooked.value(), 0), 3);
  // Linear weights spread the error evenly over the three independent
  // channels of this test image; bc7enc only uses modes 1 and 6 for opaque
  // blocks.
  CHECK(error.max <= 20);
  CHECK(error.mean < 3.5);
}

TEST_CASE("BC5 normal maps keep x and y and reconstruct z") {
  prefab::BulkStorage storage;
  const ImageData image = NormalMap(64, 64);
  for (const CookQuality quality : {CookQuality::kFast, CookQuality::kHigh}) {
    CAPTURE(static_cast<int>(quality));
    auto cooked = CookTexture(
        image, {.usage = TextureUsage::kNormal, .quality = quality}, storage);
    REQUIRE(cooked.ok());
    CHECK(cooked.value().format == TextureFormat::kBc5Unorm);
    const ImageData decoded = DecodeTextureMip(cooked.value(), 0);
    const ErrorStats xy = Compare(image, decoded, 2);
    CHECK(xy.max <= 6);
    CHECK(xy.mean < 1.5);
    // z comes from x and y; it may differ from the stored z by rounding.
    const ErrorStats xyz = Compare(image, decoded, 3);
    CHECK(xyz.max <= 12);
  }
}

TEST_CASE("Odd and tiny textures are padded to whole blocks") {
  prefab::BulkStorage storage;
  // A ramp along one colour line: what a block holds almost exactly, so the
  // padding is what is tested, not the encoder.
  ImageData image{.width = 5, .height = 3};
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 5; ++x) {
      image.pixels.insert(image.pixels.end(),
                          {static_cast<uint8_t>(40 * x + 10 * y),
                           static_cast<uint8_t>(20 * x + 5 * y), 128, 255});
    }
  }
  auto cooked = CookTexture(image, {.usage = TextureUsage::kColor}, storage);
  REQUIRE(cooked.ok());
  const prefab::Texture& texture = cooked.value();
  REQUIRE(texture.mip_count() == 3);             // 5x3, 2x1, 1x1
  CHECK(texture.mips[0].data.size() == 2 * 16);  // 2 x 1 blocks
  CHECK(texture.mips[1].width == 2);
  CHECK(texture.mips[1].data.size() == 16);
  CHECK(texture.mips[2].data.size() == 16);
  const ImageData decoded = DecodeTextureMip(texture, 0);
  CHECK(decoded.width == 5);
  CHECK(decoded.height == 3);
  CHECK(Compare(image, decoded, 4).max <= 8);
  CHECK(DecodeTextureMip(texture, 2).pixels.size() == 4);
  CHECK(DecodeTextureMip(texture, 3).pixels.empty());
}

TEST_CASE("Cooking is deterministic") {
  prefab::BulkStorage storage;
  const ImageData image = Gradient(48, 40);
  const TextureCookOptions options{.usage = TextureUsage::kColor,
                                   .quality = CookQuality::kFast};
  auto first = CookTexture(image, options, storage);
  auto second = CookTexture(image, options, storage);
  REQUIRE(first.ok());
  REQUIRE(second.ok());
  CHECK(AllBytes(first.value()) == AllBytes(second.value()));
}

TEST_CASE("EncodeTexture then StoreTexture give CookTexture's texture") {
  const ImageData gradient = Gradient(37, 23);
  const ImageData normals = NormalMap(37, 23);
  struct Case {
    const ImageData* image;
    TextureCookOptions options;
  };
  const Case cases[] = {
      {&gradient,
       {.usage = TextureUsage::kColor, .quality = CookQuality::kFast}},
      {&gradient,
       {.usage = TextureUsage::kLinear, .quality = CookQuality::kFast}},
      {&normals,
       {.usage = TextureUsage::kNormal, .quality = CookQuality::kFast}},
      {&gradient,
       {.usage = TextureUsage::kColor, .compress = false, .max_size = 16}},
      {&normals,
       {.usage = TextureUsage::kNormal,
        .compress = false,
        .generate_mips = false}},
  };
  for (const Case& test : cases) {
    CAPTURE(static_cast<int>(test.options.usage));
    CAPTURE(test.options.compress);
    prefab::BulkStorage storage;
    auto cooked = CookTexture(*test.image, test.options, storage);
    REQUIRE(cooked.ok());
    auto encoded = EncodeTexture(*test.image, test.options);
    REQUIRE(encoded.ok());
    CHECK(encoded.value().texture.mips.front().data.empty());
    const size_t blocks = storage.block_count();
    const prefab::Texture stored =
        StoreTexture(std::move(encoded).value(), storage);
    CHECK(storage.block_count() == blocks + 1);
    CHECK(stored.format == cooked.value().format);
    CHECK(stored.width == cooked.value().width);
    CHECK(stored.height == cooked.value().height);
    REQUIRE(stored.mip_count() == cooked.value().mip_count());
    for (size_t i = 0; i < stored.mips.size(); ++i) {
      CHECK(stored.mips[i].width == cooked.value().mips[i].width);
      CHECK(stored.mips[i].height == cooked.value().mips[i].height);
    }
    CHECK(AllBytes(stored) == AllBytes(cooked.value()));
  }
  CHECK(EncodeTexture(ImageData{}, {}).error() == CookError::kInvalidImage);
}

TEST_CASE("Uncompressed textures keep the mip chain bytes") {
  prefab::BulkStorage storage;
  const ImageData image = Gradient(8, 8);
  auto cooked = CookTexture(
      image, {.usage = TextureUsage::kNormal, .compress = false}, storage);
  REQUIRE(cooked.ok());
  CHECK(cooked.value().format == TextureFormat::kRgba8Unorm);
  const auto chain = GenerateMipChain(image, TextureUsage::kNormal);
  REQUIRE(cooked.value().mip_count() == static_cast<int>(chain.size()));
  for (size_t i = 0; i < chain.size(); ++i) {
    const auto& data = cooked.value().mips[i].data;
    REQUIRE(data.size() == chain[i].pixels.size());
    CHECK(std::memcmp(data.data(), chain[i].pixels.data(), data.size()) == 0);
  }
  auto single = CookTexture(image,
                            {.usage = TextureUsage::kColor,
                             .compress = false,
                             .generate_mips = false},
                            storage);
  REQUIRE(single.ok());
  CHECK(single.value().format == TextureFormat::kRgba8Srgb);
  CHECK(single.value().mip_count() == 1);
}

TEST_CASE("CookTexture rejects broken images") {
  prefab::BulkStorage storage;
  CHECK(CookTexture(ImageData{}, {}, storage).error() ==
        CookError::kInvalidImage);
  ImageData short_pixels = Solid(4, 4, 0, 0, 0);
  short_pixels.pixels.pop_back();
  CHECK(CookTexture(short_pixels, {}, storage).error() ==
        CookError::kInvalidImage);
  CHECK(storage.block_count() == 0);
}

TEST_CASE("PackOrm fills R occlusion, G roughness, B metallic") {
  const ImageData occlusion = Solid(4, 4, 100, 0, 0);
  const ImageData packed_mr =
      Solid(2, 2, 0, 50, 200);  // glTF: G rough, B metal

  const ImageData orm = PackOrm({.occlusion = &occlusion,
                                 .roughness = &packed_mr,
                                 .roughness_channel = 1,
                                 .metallic = &packed_mr,
                                 .metallic_channel = 2});
  REQUIRE(orm.width == 4);
  REQUIRE(orm.height == 4);
  for (size_t i = 0; i < orm.pixels.size(); i += 4) {
    CHECK(orm.pixels[i] == 100);
    CHECK(orm.pixels[i + 1] == 50);  // resampled from 2x2, constant
    CHECK(orm.pixels[i + 2] == 200);
    CHECK(orm.pixels[i + 3] == 255);
  }

  const ImageData roughness = Solid(3, 3, 77, 0, 0);
  const ImageData only_roughness = PackOrm({.roughness = &roughness});
  REQUIRE(only_roughness.width == 3);
  CHECK(only_roughness.pixels[0] == 255);  // missing occlusion: 1
  CHECK(only_roughness.pixels[1] == 77);
  CHECK(only_roughness.pixels[2] == 255);  // missing metallic: 1

  CHECK(PackOrm({}).pixels.empty());
}

}  // namespace
}  // namespace blue::cook
