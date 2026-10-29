#ifndef BLUE_SCENE_PLACEMENT_H_
#define BLUE_SCENE_PLACEMENT_H_

#include <glm/vec3.hpp>

#include "scene/scene.h"

namespace blue::scene {

// Returns a transform that uniformly scales a mesh with the given local
// bounds so its largest extent equals `target_size`, centres it on
// `ground_point` in X/Z and rests its lowest point on `ground_point.y`.
// Degenerate bounds (no extent) keep a scale of 1 instead of dividing by zero.
[[nodiscard]] Transform FitOnGround(glm::vec3 bounds_min, glm::vec3 bounds_max,
                                    float target_size, glm::vec3 ground_point);

}  // namespace blue::scene

#endif  // BLUE_SCENE_PLACEMENT_H_
