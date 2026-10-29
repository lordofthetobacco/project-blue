#pragma once

#include <cstddef>

namespace blue {

// Frames the CPU may record ahead of the GPU. Two keeps latency low while still
// letting CPU and GPU work overlap.
inline constexpr size_t kFramesInFlight = 2;

} // namespace blue
