#ifndef BLUE_RENDER_FRAME_CONFIG_H_
#define BLUE_RENDER_FRAME_CONFIG_H_

#include <cstddef>

namespace blue::render {

// Frames the CPU may record ahead of the GPU. Two keeps latency low while
// still letting CPU and GPU work overlap.
inline constexpr size_t kFramesInFlight = 2;

}  // namespace blue::render

#endif  // BLUE_RENDER_FRAME_CONFIG_H_
