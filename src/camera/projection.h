#ifndef BLUE_CAMERA_PROJECTION_H_
#define BLUE_CAMERA_PROJECTION_H_

#include <glm/mat4x4.hpp>

namespace blue::camera {

// Returns a perspective projection for right-handed view space (the camera
// looks down -Z) with the clip depth [0,1] of Vulkan, D3D12 and Metal, the
// near plane at 1 and infinity at 0. Pair it with a depth clear of 0 and a
// greater-than depth compare.
[[nodiscard]] glm::mat4 ReversedInfinitePerspective(float vertical_fov_radians,
                                                    float aspect,
                                                    float near_plane);

}  // namespace blue::camera

#endif  // BLUE_CAMERA_PROJECTION_H_
