#include "scene/Scene.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace blue {

glm::mat4 Transform::matrix() const
{
    return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}

} // namespace blue
