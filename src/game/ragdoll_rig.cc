#include "game/ragdoll_rig.h"

#include <span>
#include <string>

#include <glm/mat4x4.hpp>

#include "animation/transform.h"
#include "core/result.h"
#include "game/character_animator.h"
#include "physics/physics_world.h"
#include "prefab/prefab.h"

namespace blue::game {

core::Result<RagdollRig, std::string> MakeRagdollRig(
    const CharacterRig& /*rig*/, const prefab::Prefab& /*prefab*/) {
  return core::Result<RagdollRig, std::string>::Error("not implemented");
}

void PartPosesFromLocals(const RagdollRig& /*rig*/, const glm::mat4& /*root*/,
                         std::span<const animation::Transform> /*locals*/,
                         std::span<physics::Pose> /*out*/) {}

void LocalsFromPartPoses(const RagdollRig& /*rig*/,
                         std::span<const physics::Pose> /*poses*/,
                         std::span<animation::Transform> /*locals*/) {}

}  // namespace blue::game
