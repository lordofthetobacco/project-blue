#include "editor/mcp/png_encode.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// The only stb_image_write translation unit of the editor.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace blue::editor {
namespace {

// stbi_write_png_to_func callback: appends to a byte vector.
void AppendBytes(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<std::byte>*>(context);
  const auto* bytes = static_cast<const std::byte*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

RgbaImage FitWidth(RgbaImage image, int max_width) {
  const size_t expected = size_t{image.width} * image.height * 4;
  if (image.pixels.size() != expected) {
    return RgbaImage{};
  }
  if (max_width <= 0 || image.width <= static_cast<uint32_t>(max_width)) {
    for (size_t i = 3; i < image.pixels.size(); i += 4) {
      image.pixels[i] = std::byte{255};
    }
    return image;
  }
  const auto width = static_cast<uint32_t>(max_width);
  const uint32_t height = std::max<uint32_t>(
      1, static_cast<uint32_t>(uint64_t{image.height} * width / image.width));
  RgbaImage out{.width = width,
                .height = height,
                .pixels = std::vector<std::byte>(size_t{width} * height * 4)};
  // Box filter: each output pixel averages the source pixels it covers.
  for (uint32_t y = 0; y < height; ++y) {
    const uint32_t y0 =
        static_cast<uint32_t>(uint64_t{y} * image.height / height);
    const uint32_t y1 = std::max(
        y0 + 1, static_cast<uint32_t>(uint64_t{y + 1} * image.height / height));
    for (uint32_t x = 0; x < width; ++x) {
      const uint32_t x0 =
          static_cast<uint32_t>(uint64_t{x} * image.width / width);
      const uint32_t x1 = std::max(
          x0 + 1, static_cast<uint32_t>(uint64_t{x + 1} * image.width / width));
      std::array<uint32_t, 3> sum{};
      for (uint32_t sy = y0; sy < y1; ++sy) {
        const std::byte* row =
            image.pixels.data() + (size_t{sy} * image.width + x0) * 4;
        for (uint32_t sx = x0; sx < x1; ++sx, row += 4) {
          for (size_t c = 0; c < 3; ++c) {
            sum[c] += std::to_integer<uint32_t>(row[c]);
          }
        }
      }
      const uint32_t count = (y1 - y0) * (x1 - x0);
      std::byte* target = out.pixels.data() + (size_t{y} * width + x) * 4;
      for (size_t c = 0; c < 3; ++c) {
        target[c] = static_cast<std::byte>((sum[c] + count / 2) / count);
      }
      target[3] = std::byte{255};
    }
  }
  return out;
}

std::vector<std::byte> EncodePng(const RgbaImage& image) {
  std::vector<std::byte> png;
  if (image.width == 0 || image.height == 0 ||
      image.pixels.size() != size_t{image.width} * image.height * 4) {
    return png;
  }
  const int ok = stbi_write_png_to_func(
      &AppendBytes, &png, static_cast<int>(image.width),
      static_cast<int>(image.height), 4, image.pixels.data(),
      static_cast<int>(image.width * 4));
  if (ok == 0) {
    png.clear();
  }
  return png;
}

}  // namespace blue::editor
