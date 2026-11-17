#ifndef BLUE_ANIMATION_HIERARCHY_H_
#define BLUE_ANIMATION_HIERARCHY_H_

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/transform.h"

namespace blue::animation {

// One node of a transform hierarchy. `parent` is the index of the parent node
// in Hierarchy::nodes, or -1 for a root. `rest` is the node's local transform
// relative to its parent when nothing animates it.
struct Node {
  std::string name;
  int parent = -1;
  Transform rest;
};

// Rigid node hierarchy of a model (a later skin would reference these nodes
// as joints). Invariant, checked by IsValid() when a hierarchy is built and
// relied on by ComputeWorldMatrices(): every parent index is -1 or smaller
// than the index of its child, so parents precede their children.
struct Hierarchy {
  std::vector<Node> nodes;
};

// Returns true if every node's parent is -1 or an earlier node.
[[nodiscard]] bool IsValid(const Hierarchy& hierarchy);

// Returns the index of the first node called `name`, or -1 if there is none.
[[nodiscard]] int FindNode(const Hierarchy& hierarchy, std::string_view name);

// Overwrites `pose` with the rest transforms of the nodes. Processes
// min(nodes, pose.size()) entries; does not allocate.
void ResetToRest(const Hierarchy& hierarchy, std::span<Transform> pose);

// Writes world[i] = world[parent] * ToMatrix(local[i]) for every node, i.e.
// each node's transform from its local space into model space. `local` holds
// one transform per node (usually a sampled pose) and `world` receives one
// matrix per node; both must hold at least hierarchy.nodes.size() entries,
// otherwise only the leading nodes that fit in both are processed. Requires
// IsValid(hierarchy). Does not allocate.
void ComputeWorldMatrices(const Hierarchy& hierarchy,
                          std::span<const Transform> local,
                          std::span<glm::mat4> world);

}  // namespace blue::animation

#endif  // BLUE_ANIMATION_HIERARCHY_H_
