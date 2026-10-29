#include "scene/Placement.hpp"

#include <algorithm>

namespace blue {

Transform fitOnGround(glm::vec3 boundsMin, glm::vec3 boundsMax, float targetSize, glm::vec3 groundPoint)
{
    const glm::vec3 size = boundsMax - boundsMin;
    const float largest = std::max({size.x, size.y, size.z});
    const float scale = largest > 0.0f ? targetSize / largest : 1.0f;
    const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;

    Transform transform;
    transform.scale = glm::vec3(scale);
    transform.position = {groundPoint.x - center.x * scale, groundPoint.y - boundsMin.y * scale,
                          groundPoint.z - center.z * scale};
    return transform;
}

} // namespace blue
