#ifndef BLUE_ANIMATION_TRANSFORM_H_
#define BLUE_ANIMATION_TRANSFORM_H_

#include <glm/ext/quaternion_float.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace blue::animation {

// Local transform of one node as separate translation, rotation and scale,
// the form glTF animates. The matrix is T * R * S: scale first, then rotate,
// then translate. `rotation` is a unit quaternion (glm stores w, x, y, z; the
// default is the identity).
struct Transform {
  glm::vec3 translation{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};
};

// Returns T * R * S for `transform`.
[[nodiscard]] glm::mat4 ToMatrix(const Transform& transform);

}  // namespace blue::animation

#endif  // BLUE_ANIMATION_TRANSFORM_H_
