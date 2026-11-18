#include "asset/image_decoder.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "asset/animated_model.h"
#include "asset/texture_loader.h"
#include "core/path.h"
#include "material/material.h"
#include "platform/file_system.h"
#include "scene/binary_stream.h"
#include "tests/scratch_directory.h"

namespace blue::asset {
namespace {

void AppendU32(std::vector<std::byte>& out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<std::byte>((value >> shift) & 0xFFu));
  }
}

// Appends a PNG chunk (big-endian length, type, data, CRC-32 of type+data).
void AppendChunk(std::vector<std::byte>& out, std::string_view type,
                 const std::vector<std::byte>& data) {
  AppendU32(out, static_cast<uint32_t>(data.size()));
  std::vector<std::byte> typed;
  for (const char c : type) {
    typed.push_back(static_cast<std::byte>(c));
  }
  typed.insert(typed.end(), data.begin(), data.end());
  out.insert(out.end(), typed.begin(), typed.end());
  AppendU32(out, scene::Crc32(typed));
}

// Returns a PNG file with the given header fields and raw scanlines (each
// already prefixed with filter byte 0), compressed as one stored deflate
// block (scanlines must total < 64 KiB).
std::vector<std::byte> MakePng(int width, int height, int bit_depth,
                               int color_type,
                               const std::vector<uint8_t>& scanlines) {
  std::vector<std::byte> png;
  for (const int b :
       std::array<int, 8>{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A}) {
    png.push_back(static_cast<std::byte>(b));
  }
  std::vector<std::byte> header;
  AppendU32(header, static_cast<uint32_t>(width));
  AppendU32(header, static_cast<uint32_t>(height));
  for (const int b : {bit_depth, color_type, 0, 0, 0}) {
    header.push_back(static_cast<std::byte>(b));
  }
  AppendChunk(png, "IHDR", header);

  // zlib: header, one final stored block, Adler-32.
  std::vector<std::byte> zlib = {std::byte{0x78}, std::byte{0x01},
                                 std::byte{0x01}};
  const auto length = static_cast<uint16_t>(scanlines.size());
  for (const uint16_t value : {length, static_cast<uint16_t>(~length)}) {
    zlib.push_back(static_cast<std::byte>(value & 0xFFu));
    zlib.push_back(static_cast<std::byte>(value >> 8));
  }
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t byte : scanlines) {
    zlib.push_back(static_cast<std::byte>(byte));
    a = (a + byte) % 65521u;
    b = (b + a) % 65521u;
  }
  AppendU32(zlib, (b << 16) | a);
  AppendChunk(png, "IDAT", zlib);
  AppendChunk(png, "IEND", {});
  return png;
}

// Returns a Radiance .hdr file of `width` x `height` flat RGBE pixels.
std::vector<std::byte> MakeHdr(int width, int height,
                               const std::vector<uint8_t>& rgbe) {
  const std::string header = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " +
                             std::to_string(height) + " +X " +
                             std::to_string(width) + "\n";
  std::vector<std::byte> file;
  for (const char c : header) {
    file.push_back(static_cast<std::byte>(c));
  }
  for (const uint8_t byte : rgbe) {
    file.push_back(static_cast<std::byte>(byte));
  }
  return file;
}

std::string GlockPath() {
  return core::JoinPath(BLUE_SAMPLES_DIR, "glock/glock17.glb");
}

}  // namespace

TEST_CASE("DecodeImage reduces a 16-bit RGB PNG to RGBA8") {
  // Two pixels: (ffff, 8000, 0000) and (1234, 00ff, abcd).
  const std::vector<uint8_t> scanlines = {0,    0xFF, 0xFF, 0x80, 0x00,
                                          0x00, 0x00, 0x12, 0x34, 0x00,
                                          0xFF, 0xAB, 0xCD};
  const std::vector<std::byte> png = MakePng(2, 1, 16, 2, scanlines);

  const auto image = DecodeImage(png);

  REQUIRE(image.ok());
  CHECK(image.value().width == 2);
  CHECK(image.value().height == 1);
  CHECK(image.value().channels == 4);
  // stb_image keeps the high byte.
  CHECK(image.value().pixels ==
        std::vector<uint8_t>{0xFF, 0x80, 0x00, 0xFF, 0x12, 0x00, 0xAB, 0xFF});
}

TEST_CASE("DecodeImage expands grey to RGB with opaque alpha, top row first") {
  const std::vector<uint8_t> scanlines = {0, 10, 0, 200};  // 1 x 2, grey
  const auto image = DecodeImage(MakePng(1, 2, 8, 0, scanlines));
  REQUIRE(image.ok());
  CHECK(image.value().pixels ==
        std::vector<uint8_t>{10, 10, 10, 255, 200, 200, 200, 255});
}

TEST_CASE("DecodeImage rejects garbage and empty input") {
  const std::string_view garbage = "definitely not an image";
  CHECK((DecodeImage(std::as_bytes(std::span(garbage))).error() ==
         ImageError::kDecodeFailed));
  CHECK((DecodeImage({}).error() == ImageError::kDecodeFailed));
}

TEST_CASE("LoadImageFile decodes a JPEG and reports missing files") {
  const auto color = LoadImageFile(
      core::JoinPath(BLUE_SAMPLES_DIR, "textures/metal032/color.jpg"));
  REQUIRE(color.ok());
  CHECK(color.value().width == 1024);
  CHECK(color.value().height == 1024);
  CHECK(color.value().pixels.size() == size_t{1024} * 1024 * 4);

  const auto missing = LoadImageFile(
      core::JoinPath(platform::TempDirectory(), "blue_no_such_image.png"));
  CHECK((missing.error() == ImageError::kFileNotFound));
}

TEST_CASE("LoadHdrFile decodes Radiance RGBE to linear floats") {
  const testing::ScratchDirectory dir("hdr");
  const std::string path = dir.Child("tiny.hdr");
  // 2 x 1: (1, 0.5, 4) and (0, 0, 0).
  const std::vector<std::byte> file =
      MakeHdr(2, 1, {128, 64, 255, 129, 0, 0, 0, 0});
  REQUIRE(platform::WriteFileAtomic(path, file).ok());

  const auto image = LoadHdrFile(path);

  REQUIRE(image.ok());
  CHECK(image.value().width == 2);
  CHECK(image.value().height == 1);
  REQUIRE(image.value().pixels.size() == 6);
  CHECK(image.value().pixels[0] == doctest::Approx(1.0f));
  CHECK(image.value().pixels[1] == doctest::Approx(0.5f));
  CHECK(image.value().pixels[2] == doctest::Approx(255.0f / 128.0f));
  CHECK(image.value().pixels[3] == 0.0f);
}

TEST_CASE("LoadHdrFile reads the sample sky panorama") {
  const auto start = std::chrono::steady_clock::now();
  const auto image = LoadHdrFile(core::JoinPath(
      BLUE_SAMPLES_DIR, "env/kloofendal_48d_partly_cloudy_puresky_2k.hdr"));
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  MESSAGE("2048 x 1024 HDR decoded in ", elapsed.count(), " ms");
  REQUIRE(image.ok());
  CHECK(image.value().width == 2048);
  CHECK(image.value().height == 1024);
  float brightest = 0.0f;
  for (const float value : image.value().pixels) {
    brightest = std::max(brightest, value);
  }
  // The sun is far brighter than anything an LDR image can hold.
  CHECK(brightest > 100.0f);
}

TEST_CASE("LoadTextures decodes the Glock's embedded textures") {
  const auto model = ImportAnimatedModel(GlockPath());
  REQUIRE(model.ok());
  std::vector<material::TextureRef> refs;
  for (const material::MaterialDesc& desc : model.value().materials) {
    refs.push_back(desc.base_color_texture);
    refs.push_back(desc.metallic_roughness_texture);
    refs.push_back(desc.normal_texture);
  }
  refs.push_back({.path = GlockPath(), .embedded_index = 999});
  refs.push_back({});

  const auto start = std::chrono::steady_clock::now();
  const auto images = LoadTextures(refs);
  const auto elapsed = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start);
  MESSAGE((refs.size() - 2), " Glock textures decoded in ", elapsed.count(),
          " ms");

  REQUIRE(images.size() == refs.size());
  for (size_t i = 0; i + 2 < images.size(); ++i) {
    CAPTURE(i);
    REQUIRE(images[i].ok());
    CHECK(images[i].value().width == 2048);
    CHECK(images[i].value().height == 2048);
    CHECK(images[i].value().channels == 4);
  }
  CHECK((images[images.size() - 2].error() == ImageError::kNoTexture));
  CHECK((images.back().error() == ImageError::kNoTexture));

  // The normal map points mostly out of the surface: blue near 1.
  const ImageData& normal = images[2].value();
  double blue = 0.0;
  for (size_t i = 2; i < normal.pixels.size(); i += 4) {
    blue += normal.pixels[i];
  }
  CHECK(blue / static_cast<double>(normal.pixels.size() / 4) > 200.0);

  // A single file texture through LoadTexture.
  const auto file =
      LoadTexture({.path = core::JoinPath(BLUE_SAMPLES_DIR,
                                          "textures/metal032/metalness.jpg")});
  REQUIRE(file.ok());
  CHECK(file.value().width == 1024);
}

TEST_CASE("PackMetallicRoughness puts roughness in G and metallic in B") {
  ImageData roughness{.width = 2, .height = 2};
  roughness.pixels = {10, 0, 0, 255, 20, 0, 0, 255,
                      30, 0, 0, 255, 40, 0, 0, 255};
  ImageData metallic{.width = 1, .height = 1};
  metallic.pixels = {77, 0, 0, 255};

  SUBCASE("both maps, smaller one resampled") {
    const ImageData packed = PackMetallicRoughness(&roughness, &metallic);
    CHECK(packed.width == 2);
    CHECK(packed.height == 2);
    CHECK(packed.pixels == std::vector<uint8_t>{0, 10, 77, 255, 0, 20, 77, 255,
                                                0, 30, 77, 255, 0, 40, 77,
                                                255});
  }
  SUBCASE("a missing map is white so the factor applies") {
    const ImageData packed = PackMetallicRoughness(nullptr, &metallic);
    CHECK(packed.pixels == std::vector<uint8_t>{0, 255, 77, 255});
    const ImageData rough_only = PackMetallicRoughness(&roughness, nullptr);
    CHECK(rough_only.pixels[1] == 10);
    CHECK(rough_only.pixels[2] == 255);
  }
  SUBCASE("no maps") {
    CHECK(PackMetallicRoughness(nullptr, nullptr).pixels.empty());
  }
  SUBCASE("bilinear upsampling of a 2 x 1 map") {
    ImageData wide{.width = 2, .height = 1};
    wide.pixels = {0, 0, 0, 255, 200, 0, 0, 255};
    ImageData big{.width = 4, .height = 1};
    big.pixels.assign(16, 255);
    const ImageData packed = PackMetallicRoughness(&wide, &big);
    // Texel centres at 1/8, 3/8, 5/8, 7/8 of the 2-texel image.
    CHECK(packed.pixels[1] == 0);
    CHECK(packed.pixels[5] == 50);
    CHECK(packed.pixels[9] == 150);
    CHECK(packed.pixels[13] == 200);
  }
}

}  // namespace blue::asset
