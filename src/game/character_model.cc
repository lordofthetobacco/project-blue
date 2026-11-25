#include "game/character_model.h"

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "game/character_animator.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "render/scene_view.h"
#include "weapon/firearm.h"
#include "world/prefab_cache.h"

namespace blue::game {

HeldWeapon HeldWeaponOf(weapon::WeaponKind kind) {
  switch (kind) {
    case weapon::WeaponKind::kPistol:
      return HeldWeapon::kPistol;
    case weapon::WeaponKind::kRifle:
      return HeldWeapon::kRifle;
  }
  return HeldWeapon::kRifle;
}

std::optional<CharacterModel> CharacterModel::Load(world::PrefabCache& cache,
                                                   std::string_view path) {
  auto loaded = cache.Get(path);
  if (!loaded.ok()) {
    core::log::Warn(
        "player model {} not loaded ({}); players are drawn as capsules", path,
        prefab::ToString(loaded.error()));
    return std::nullopt;
  }
  const prefab::Prefab& prefab = cache.prefab(loaded.value());
  auto rig = MakeCharacterRig(prefab.hierarchy, prefab.sockets);
  if (!rig.ok()) {
    core::log::Warn(
        "player model {} is no animatable model ({}); players are drawn as "
        "capsules",
        path, rig.error());
    return std::nullopt;
  }
  return CharacterModel(cache, loaded.value(), std::move(rig).value());
}

CharacterModel::CharacterModel(const world::PrefabCache& cache,
                               world::PrefabId prefab, CharacterRig rig)
    : rig_(std::move(rig)), pose_(cache, prefab) {}

void CharacterModel::AppendDraws(const CharacterAnimator& animator,
                                 std::vector<render::DrawItem>& out) {
  pose_.SetLocalPose(animator.locals());
  pose_.AppendDraws(animator.root(), out);
}

}  // namespace blue::game
