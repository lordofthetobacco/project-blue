#ifndef BLUE_GAME_CHARACTER_MODEL_H_
#define BLUE_GAME_CHARACTER_MODEL_H_

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

#include "game/character_animator.h"
#include "render/scene_view.h"
#include "weapon/firearm.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"

namespace blue::game {

// The player model file in the assets directory (blue-pack samples).
inline constexpr std::string_view kCharacterModelFile = "kyle.pbo";

// Returns how a weapon of `kind` is held.
[[nodiscard]] HeldWeapon HeldWeaponOf(weapon::WeaponKind kind);

// Draws characters with the player model: poses its prefab from a
// CharacterAnimator built with rig() and appends one draw item per body
// part. One model serves any number of characters (the draw items carry
// the matrices). Borrows `cache`, which must outlive it. Copyable. Not
// thread-safe.
class CharacterModel {
 public:
  // Loads the model at `path` through `cache` and reads its rig. Returns
  // nullopt with a warning logged when the file is missing or is no valid
  // player model (the caller then draws its fallback).
  [[nodiscard]] static std::optional<CharacterModel> Load(
      world::PrefabCache& cache, std::string_view path);

  CharacterModel(const world::PrefabCache& cache, world::PrefabId prefab,
                 CharacterRig rig);

  [[nodiscard]] const CharacterRig& rig() const { return rig_; }
  [[nodiscard]] world::PrefabId prefab() const { return pose_.prefab(); }

  // Poses the model as `animator` (made with rig()) and appends its draws
  // placed by animator.root(). Does not allocate beyond `out`'s growth.
  void AppendDraws(const CharacterAnimator& animator,
                   std::vector<render::DrawItem>& out);

  // Poses the model with `locals` (one transform per node of rig()'s
  // hierarchy, as CharacterAnimator::locals()) and appends its draws placed
  // by `root`. Does not allocate beyond `out`'s growth.
  void AppendDraws(const glm::mat4& root,
                   std::span<const animation::Transform> locals,
                   std::vector<render::DrawItem>& out);

 private:
  CharacterRig rig_;
  world::PrefabPose pose_;
};

}  // namespace blue::game

#endif  // BLUE_GAME_CHARACTER_MODEL_H_
