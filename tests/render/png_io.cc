#include "tests/render/png_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "asset/image_decoder.h"

// The only stb_image_write translation unit of the render tests.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace blue::render_test {
namespace {

// stbi_write_png_to_func callback: appends to a byte vector.
void AppendBytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<char>*>(context);
  const auto* bytes = static_cast<const char*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

// A 5 x 7 glyph: seven rows, bit 4 the leftmost column.
using Glyph = std::array<uint8_t, 7>;

struct GlyphEntry {
  char c;
  Glyph rows;
};

constexpr std::array<GlyphEntry, 40> kFont{{
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}},
    {'-', {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}},
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
}};

constexpr uint32_t kGlyphScale = 3;
constexpr uint32_t kLabelBand = 7 * kGlyphScale + 12;
constexpr uint32_t kGap = 8;
constexpr uint8_t kBackground = 40;

const Glyph* FindGlyph(char c) {
  const char upper = static_cast<char>(
      std::toupper(static_cast<unsigned char>(c)));
  for (const GlyphEntry& entry : kFont) {
    if (entry.c == upper) {
      return &entry.rows;
    }
  }
  return nullptr;
}

void SetPixel(RgbaImage& image, uint32_t x, uint32_t y, uint8_t value) {
  if (x >= image.width || y >= image.height) {
    return;
  }
  const size_t i = (static_cast<size_t>(y) * image.width + x) * 4;
  image.rgba[i] = value;
  image.rgba[i + 1] = value;
  image.rgba[i + 2] = value;
  image.rgba[i + 3] = 255;
}

// Draws `text` in white with its top-left corner at (x, y).
void DrawText(RgbaImage& image, uint32_t x, uint32_t y,
              const std::string& text) {
  for (const char c : text) {
    const Glyph* glyph = FindGlyph(c);
    if (glyph != nullptr) {
      for (uint32_t row = 0; row < 7; ++row) {
        for (uint32_t column = 0; column < 5; ++column) {
          if (((*glyph)[row] >> (4 - column) & 1) == 0) {
            continue;
          }
          for (uint32_t sy = 0; sy < kGlyphScale; ++sy) {
            for (uint32_t sx = 0; sx < kGlyphScale; ++sx) {
              SetPixel(image, x + column * kGlyphScale + sx,
                       y + row * kGlyphScale + sy, 255);
            }
          }
        }
      }
    }
    x += 6 * kGlyphScale;
  }
}

// Copies `source` (alpha forced to 255) to (x, y) of `target`.
void Blit(RgbaImage& target, uint32_t x, uint32_t y, const RgbaImage& source) {
  for (uint32_t row = 0; row < source.height && y + row < target.height;
       ++row) {
    for (uint32_t column = 0;
         column < source.width && x + column < target.width; ++column) {
      const size_t from = (static_cast<size_t>(row) * source.width + column) * 4;
      const size_t to =
          (static_cast<size_t>(y + row) * target.width + x + column) * 4;
      target.rgba[to] = source.rgba[from];
      target.rgba[to + 1] = source.rgba[from + 1];
      target.rgba[to + 2] = source.rgba[from + 2];
      target.rgba[to + 3] = 255;
    }
  }
}

}  // namespace

bool WritePng(const std::string& path, const RgbaImage& image) {
  if (!image.valid()) {
    return false;
  }
  std::error_code error;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
  }
  std::vector<char> png;
  const int ok = stbi_write_png_to_func(
      AppendBytes, &png, static_cast<int>(image.width),
      static_cast<int>(image.height), 4, image.rgba.data(),
      static_cast<int>(image.width * 4));
  if (ok == 0) {
    return false;
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(png.data(), static_cast<std::streamsize>(png.size()));
  return static_cast<bool>(file);
}

std::optional<RgbaImage> ReadPng(const std::string& path) {
  auto decoded = asset::LoadImageFile(path);
  if (!decoded.ok()) {
    return std::nullopt;
  }
  asset::ImageData& data = decoded.value();
  RgbaImage image{.width = static_cast<uint32_t>(data.width),
                  .height = static_cast<uint32_t>(data.height),
                  .rgba = std::move(data.pixels)};
  if (!image.valid()) {
    return std::nullopt;
  }
  return image;
}

RgbaImage MakeContactSheet(std::span<const LabeledImage> images,
                           uint32_t columns) {
  columns = std::max(columns, 1u);
  uint32_t cell_width = 1;
  uint32_t cell_height = 1;
  for (const LabeledImage& entry : images) {
    cell_width = std::max(cell_width, entry.image.width);
    cell_height = std::max(cell_height, entry.image.height);
  }
  const auto count = static_cast<uint32_t>(images.size());
  const uint32_t used_columns = std::max(std::min(columns, count), 1u);
  const uint32_t rows = (count + columns - 1) / columns;
  RgbaImage sheet{
      .width = kGap + used_columns * (cell_width + kGap),
      .height = kGap + std::max(rows, 1u) * (kLabelBand + cell_height + kGap)};
  sheet.rgba.assign(static_cast<size_t>(sheet.width) * sheet.height * 4,
                    kBackground);
  for (size_t i = 3; i < sheet.rgba.size(); i += 4) {
    sheet.rgba[i] = 255;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t x = kGap + (i % columns) * (cell_width + kGap);
    const uint32_t y = kGap + (i / columns) * (kLabelBand + cell_height + kGap);
    DrawText(sheet, x, y + (kLabelBand - 7 * kGlyphScale) / 2,
             images[i].label);
    Blit(sheet, x, y + kLabelBand, images[i].image);
  }
  return sheet;
}

}  // namespace blue::render_test
