#ifndef BLUE_WEAPON_WEAPON_ITEM_H_
#define BLUE_WEAPON_WEAPON_ITEM_H_

#include <cstdint>

#include "weapon/firearm.h"
#include "weapon/parts/weapon_build.h"

namespace blue::weapon {

// Names a weapon definition (what the game derives from a weapon: model
// parts, calibration, fire animation, spec and feel; see weapon_def.h)
// inside the game's WeaponLibrary. Small and stable for the library's life.
enum class WeaponDefId : uint32_t {};

// One weapon as an object of the game: its parts, the definition derived
// from them and its mechanism's state. Carried in a Loadout slot or lying in
// the world (DroppedWeapons); moves between them whole. Copyable.
struct WeaponItem {
  WeaponDefId def{};
  Firearm firearm;
  // The magnification chosen for the weapon's scope, kept while it is
  // carried, dropped and picked up again; 0 for a weapon without a scope.
  float scope_magnification = 0.0f;
  // The distance (m) the sights are zeroed at (a scope's elevation turret,
  // or the zero steps of a part-built weapon's sights), remembered like the
  // magnification; 0 for a weapon whose zero cannot be set.
  float scope_zero = 0.0f;
  // The parts the weapon is built from; empty for a single-piece weapon the
  // part catalogue does not describe. `def` is derived from it.
  parts::WeaponBuild build;
  // Multiplies the Loadout's holster and draw times for this weapon (its
  // handling; 1 = the tuning's times).
  float switch_time_scale = 1.0f;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_ITEM_H_
