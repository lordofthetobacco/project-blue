#ifndef BLUE_PHYSICS_INTERNAL_RAGDOLL_DATA_H_
#define BLUE_PHYSICS_INTERNAL_RAGDOLL_DATA_H_

#include <vector>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
// clang-format on

#include "physics/ragdoll.h"

// The private part of RagdollTemplate: what Ragdoll::Create needs to build an
// instance. Only physics sources include this header; it may hold Jolt types.

namespace blue::physics {

struct RagdollTemplate::Data {
  std::vector<int> parents;
  std::vector<Pose> rest_poses;
  std::vector<CookedShape> shapes;
  // Bodies at their rest poses, constraints in world space, group filter,
  // masses and priorities, shared by every instance.
  JPH::Ref<JPH::RagdollSettings> settings;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_INTERNAL_RAGDOLL_DATA_H_
