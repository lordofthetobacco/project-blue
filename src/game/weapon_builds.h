#ifndef BLUE_GAME_WEAPON_BUILDS_H_
#define BLUE_GAME_WEAPON_BUILDS_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "world/prefab_cache.h"

// The game's weapon part catalogue and the weapons built from it: the glue
// between the catalogue (weapon/parts/), the prefab cache that holds the
// part models and the weapon library that holds the definitions.

namespace blue::game {

// Where the catalogue lives under the asset root.
inline constexpr std::string_view kCatalogDirectory = "weapons/catalog";

// Makes weapon definitions from part builds. Part prefabs are loaded through
// `cache` from paths relative to `asset_root`; definitions go into the
// caller's library, one per distinct build (WeaponLibrary::FindBuild).
//
// Borrows `cache`, which must outlive it. Allocates when a catalogue is
// loaded and when a build is seen for the first time, not per frame.
// Neither copyable nor movable. Not thread-safe.
class WeaponBuilds {
 public:
  WeaponBuilds(world::PrefabCache& cache, std::string asset_root);
  WeaponBuilds(const WeaponBuilds&) = delete;
  WeaponBuilds& operator=(const WeaponBuilds&) = delete;

  // Loads <asset root>/kCatalogDirectory. A missing directory or a broken
  // file is logged and leaves no catalogue: the game then has its
  // single-piece weapons only. Returns whether a catalogue is loaded.
  bool LoadCatalog();
  // Uses `catalog` from now on (tests, or a catalogue read elsewhere).
  void SetCatalog(weapon::parts::PartCatalog catalog);
  // Null without a catalogue.
  [[nodiscard]] const weapon::parts::PartCatalog* catalog() const {
    return catalog_ ? &*catalog_ : nullptr;
  }

  // Returns the definition of `build` from `library`, assembling it the
  // first time; an error message (not logged) when it cannot be assembled
  // or there is no catalogue.
  core::Result<weapon::WeaponDefId, std::string> DefFor(
      const weapon::parts::WeaponBuild& build, weapon::WeaponLibrary& library);

  // Returns the definition of preset `id`, or nullopt when there is no such
  // preset, its root part has no model (a single-piece weapon known by its
  // prefab only), or it cannot be assembled (logged once per preset).
  std::optional<weapon::WeaponDefId> PresetDef(std::string_view id,
                                               weapon::WeaponLibrary& library);

  // Re-assembles every build definition of `library` in place (its part
  // prefabs were reloaded). One that fails keeps its previous data (logged).
  // Returns how many were rebuilt.
  int RebuildDefs(weapon::WeaponLibrary& library);

 private:
  // Assembles `build` with models from cache_.
  core::Result<weapon::WeaponDef, std::string> Assemble(
      const weapon::parts::WeaponBuild& build);

  world::PrefabCache& cache_;
  std::string asset_root_;
  std::optional<weapon::parts::PartCatalog> catalog_;
  std::vector<std::string> reported_presets_;  // failures already logged
};

}  // namespace blue::game

#endif  // BLUE_GAME_WEAPON_BUILDS_H_
