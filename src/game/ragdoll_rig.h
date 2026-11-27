#ifndef BLUE_GAME_RAGDOLL_RIG_H_
#define BLUE_GAME_RAGDOLL_RIG_H_

#include <array>
#include <span>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

#include "animation/transform.h"
#include "core/result.h"
#include "game/character_animator.h"
#include "physics/physics_world.h"
#include "physics/ragdoll.h"
#include "prefab/prefab.h"

// The ragdoll skeleton of the player model: 13 rigid parts (pelvis, chest,
// head, two arms of two parts, two legs of three parts) with their joints and
// limits, derived from the model's rest offsets and mesh bounds, plus the
// conversion between the animator's local node transforms and part poses.

namespace blue::game {

// The parts of the ragdoll, in RagdollDesc order.
enum class RagdollPart : int {
  kPelvis,
  kChest,
  kHead,
  kLeftUpperArm,
  kLeftForearm,
  kRightUpperArm,
  kRightForearm,
  kLeftThigh,
  kLeftShin,
  kLeftFoot,
  kRightThigh,
  kRightShin,
  kRightFoot,
};
inline constexpr int kRagdollPartCount = 13;

// The ragdoll skeleton of one player model.
struct RagdollRig {
  physics::RagdollDesc desc;  // In RagdollPart order.
  // The animator joint that is each part's frame node.
  std::array<Joint, kRagdollPartCount> part_joint{};
  // The model's node index of each part's frame node.
  std::array<int, kRagdollPartCount> part_node{};
  // Per node: the part it belongs to, or -1 when it rides on its parent
  // (sockets, wrist, toe).
  std::vector<int> node_part;
  std::vector<int> node_parent;  // Per node: parent node, -1 for the root.
};

// Builds the ragdoll skeleton of `prefab` (the player model `rig` was made
// from). Returns an error text when the model lacks what the table needs.
[[nodiscard]] core::Result<RagdollRig, std::string> MakeRagdollRig(
    const CharacterRig& rig, const prefab::Prefab& prefab);

// Writes into `out` (kRagdollPartCount entries) the world pose of every part
// when the model is posed by `locals` and placed by `root`.
void PartPosesFromLocals(const RagdollRig& rig, const glm::mat4& root,
                         std::span<const animation::Transform> locals,
                         std::span<physics::Pose> out);

// Rewrites `locals` so that, drawn with an IDENTITY root, every part's node
// sits at its pose in `poses` (world space); other nodes keep their local
// transform.
void LocalsFromPartPoses(const RagdollRig& rig,
                         std::span<const physics::Pose> poses,
                         std::span<animation::Transform> locals);

}  // namespace blue::game

#endif  // BLUE_GAME_RAGDOLL_RIG_H_
