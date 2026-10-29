#pragma once

#include "scene/Scene.hpp"

#include <glm/vec3.hpp>

namespace blue {

// Uniformly scales a mesh with the given local bounds so its largest extent equals `targetSize`,
// centres it on `groundPoint` in X/Z and rests its lowest point on `groundPoint.y`.
// Degenerate bounds (no extent) keep a scale of 1 instead of dividing by zero.
[[nodiscard]] Transform fitOnGround(glm::vec3 boundsMin, glm::vec3 boundsMax, float targetSize, glm::vec3 groundPoint);

} // namespace blue
