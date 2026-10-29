#include "camera/projection.h"

#include <cmath>

namespace blue::camera {

glm::mat4 ReversedInfinitePerspective(float vertical_fov_radians, float aspect,
                                      float near_plane) {
  // Float precision is densest near 0, so storing 1/z-like depth with
  // infinity at 0 spreads the precision evenly over the range instead of
  // wasting it next to the near plane.
  const float f = 1.0f / std::tan(vertical_fov_radians * 0.5f);

  glm::mat4 projection(0.0f);
  projection[0][0] = f / aspect;
  projection[1][1] = f;
  projection[2][3] = -1.0f;
  projection[3][2] = near_plane;
  return projection;
}

}  // namespace blue::camera
