#ifndef BLUE_EDITOR_MCP_PNG_ENCODE_H_
#define BLUE_EDITOR_MCP_PNG_ENCODE_H_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// PNG encoding of RGBA8 images for viewport_screenshot (stb_image_write).
// Pure; no GPU.

namespace blue::editor {

// An RGBA8 image, top row first.
struct RgbaImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<std::byte> pixels;  // width * height * 4
};

// Returns `image` scaled down (box filter, aspect kept) so that it is at
// most `max_width` pixels wide, or unchanged when it already is or
// `max_width` <= 0. Alpha is forced to opaque.
[[nodiscard]] RgbaImage FitWidth(RgbaImage image, int max_width);

// Returns the PNG file bytes of `image`; empty when the image is empty or
// encoding fails.
[[nodiscard]] std::vector<std::byte> EncodePng(const RgbaImage& image);

}  // namespace blue::editor

#endif  // BLUE_EDITOR_MCP_PNG_ENCODE_H_
