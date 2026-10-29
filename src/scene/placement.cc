#include "scene/placement.h"

#include <algorithm>

namespace blue::scene {

Transform FitOnGround(glm::vec3 bounds_min, glm::vec3 bounds_max,
                      float target_size, glm::vec3 ground_point) {
  const glm::vec3 size = bounds_max - bounds_min;
  const float largest = std::max({size.x, size.y, size.z});
  const float scale = largest > 0.0f ? target_size / largest : 1.0f;
  const glm::vec3 center = (bounds_min + bounds_max) * 0.5f;

  Transform transform;
  transform.scale = glm::vec3(scale);
  transform.position = {ground_point.x - center.x * scale,
                        ground_point.y - bounds_min.y * scale,
                        ground_point.z - center.z * scale};
  return transform;
}

}  // namespace blue::scene
