// Tests of game::MakeRagdollRig and the pose conversions (game/ragdoll_rig.h):
// 13 parts of 70 +- 3 kg, left/right mirror, shapes inside the mesh bounds,
// locals -> parts -> locals round trips for standing, crouching and aiming,
// the template creates, CharacterModel::AppendDraws overloads agree. Cases are
// added by the rig work package.

#include "game/ragdoll_rig.h"

#include <doctest/doctest.h>

#include "game/character_model.h"

namespace blue::game {
namespace {}  // namespace
}  // namespace blue::game
