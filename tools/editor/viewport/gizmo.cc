#include "editor/viewport/gizmo.h"

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

namespace blue::editor {

animation::Transform LocalFromWorld(const glm::mat4& parent_world,
                                    const glm::mat4& world) {
  const glm::mat4 local = glm::inverse(parent_world) * world;
  animation::Transform transform;
  transform.translation = glm::vec3(local[3]);
  glm::mat3 basis(local);
  for (int axis = 0; axis < 3; ++axis) {
    transform.scale[axis] = glm::length(basis[axis]);
  }
  // A mirroring matrix keeps a proper rotation with a negative X scale.
  if (glm::determinant(basis) < 0.0f) {
    transform.scale.x = -transform.scale.x;
  }
  for (int axis = 0; axis < 3; ++axis) {
    if (transform.scale[axis] != 0.0f) {
      basis[axis] /= transform.scale[axis];
    }
  }
  // Re-orthonormalise (drops shear) before converting to a quaternion; a
  // degenerate basis (zero scale) keeps the identity rotation.
  const glm::vec3 y_rest = basis[1] - basis[0] * glm::dot(basis[0], basis[1]);
  if (glm::length(basis[0]) > 1.0e-6f && glm::length(y_rest) > 1.0e-6f) {
    const glm::vec3 x = glm::normalize(basis[0]);
    const glm::vec3 y = glm::normalize(basis[1] - x * glm::dot(x, basis[1]));
    transform.rotation =
        glm::normalize(glm::quat_cast(glm::mat3(x, y, glm::cross(x, y))));
  }
  return transform;
}

}  // namespace blue::editor
