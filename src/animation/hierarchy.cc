#include "animation/hierarchy.h"

#include <algorithm>
#include <cstddef>

namespace blue::animation {

bool IsValid(const Hierarchy& hierarchy) {
  for (size_t i = 0; i < hierarchy.nodes.size(); ++i) {
    const int parent = hierarchy.nodes[i].parent;
    if (parent < -1 || (parent >= 0 && static_cast<size_t>(parent) >= i)) {
      return false;
    }
  }
  return true;
}

int FindNode(const Hierarchy& hierarchy, std::string_view name) {
  for (size_t i = 0; i < hierarchy.nodes.size(); ++i) {
    if (hierarchy.nodes[i].name == name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ResetToRest(const Hierarchy& hierarchy, std::span<Transform> pose) {
  const size_t count = std::min(hierarchy.nodes.size(), pose.size());
  for (size_t i = 0; i < count; ++i) {
    pose[i] = hierarchy.nodes[i].rest;
  }
}

void ComputeWorldMatrices(const Hierarchy& hierarchy,
                          std::span<const Transform> local,
                          std::span<glm::mat4> world) {
  const size_t count =
      std::min({hierarchy.nodes.size(), local.size(), world.size()});
  for (size_t i = 0; i < count; ++i) {
    const glm::mat4 matrix = ToMatrix(local[i]);
    const int parent = hierarchy.nodes[i].parent;
    // IsValid() guarantees parent < i, so world[parent] is already final.
    world[i] =
        parent < 0 ? matrix : world[static_cast<size_t>(parent)] * matrix;
  }
}

}  // namespace blue::animation
