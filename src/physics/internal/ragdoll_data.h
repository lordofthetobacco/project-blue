#ifndef BLUE_PHYSICS_INTERNAL_RAGDOLL_DATA_H_
#define BLUE_PHYSICS_INTERNAL_RAGDOLL_DATA_H_

#include <vector>

#include "physics/ragdoll.h"

// The private part of RagdollTemplate: what Ragdoll::Create needs to build an
// instance. Only physics sources include this header; it may hold Jolt types.

namespace blue::physics {

struct RagdollTemplate::Data {
  std::vector<int> parents;
  std::vector<Pose> rest_poses;
  std::vector<CookedShape> shapes;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_INTERNAL_RAGDOLL_DATA_H_
