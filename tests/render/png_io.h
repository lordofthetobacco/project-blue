#ifndef BLUE_TESTS_RENDER_PNG_IO_H_
#define BLUE_TESTS_RENDER_PNG_IO_H_

#include <optional>
#include <span>
#include <string>

#include "tests/render/image_compare.h"

namespace blue::render_test {

// Writes `image` as an RGBA PNG at `path` (parent directories are created).
// Returns false on failure.
[[nodiscard]] bool WritePng(const std::string& path, const RgbaImage& image);

// Reads the PNG at `path` as RGBA8; nullopt if it is missing or invalid.
[[nodiscard]] std::optional<RgbaImage> ReadPng(const std::string& path);

// One tile of a contact sheet.
struct LabeledImage {
  std::string label;
  RgbaImage image;
};

// Returns `images` tiled `columns` per row (cells sized to the largest
// image), each with its label (upper case; A-Z, 0-9, '_', '-', '.') in a
// band above it, on a dark grey background.
[[nodiscard]] RgbaImage MakeContactSheet(std::span<const LabeledImage> images,
                                         uint32_t columns);

}  // namespace blue::render_test

#endif  // BLUE_TESTS_RENDER_PNG_IO_H_
