#include "animation/transform.h"

#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace blue::animation {

glm::mat4 ToMatrix(const Transform& transform) {
  glm::mat4 matrix = glm::mat4_cast(transform.rotation);
  matrix[0] *= transform.scale.x;
  matrix[1] *= transform.scale.y;
  matrix[2] *= transform.scale.z;
  matrix[3] = glm::vec4(transform.translation, 1.0f);
  return matrix;
}

}  // namespace blue::animation
