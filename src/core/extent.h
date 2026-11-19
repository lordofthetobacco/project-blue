#ifndef BLUE_CORE_EXTENT_H_
#define BLUE_CORE_EXTENT_H_

#include <cstdint>

namespace blue::core {

// A size in pixels, e.g. of a window or an image.
struct Extent2D {
  uint32_t width = 0;
  uint32_t height = 0;

  friend bool operator==(const Extent2D&, const Extent2D&) = default;
};

}  // namespace blue::core

#endif  // BLUE_CORE_EXTENT_H_
