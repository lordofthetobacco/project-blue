#pragma once

#include <glm/mat4x4.hpp>

namespace blue {

// Right-handed view space (camera looks down -Z), Vulkan clip depth [0,1] with the near plane at 1
// and infinity at 0. Pair it with a depth clear of 0 and a GREATER compare op.
[[nodiscard]] glm::mat4 reversedInfinitePerspective(float verticalFovRadians, float aspect, float nearPlane);

} // namespace blue
