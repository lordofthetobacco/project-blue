#ifndef BLUE_WEAPON_PARTS_WEAPON_STATS_H_
#define BLUE_WEAPON_PARTS_WEAPON_STATS_H_

#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"

namespace blue::weapon::parts {

// What a build adds up to. Every figure is derived from the installed parts
// by ComputeStats; nothing here is stored in the catalogue as such.
struct WeaponStats {
  float weight = 0.0f;           // kg, sum of every part
  float ergonomics = 0.0f;       // root + sum of deltas, clamped to [0, 100]
  float accuracy_moa = 0.0f;     // barrel MOA x (1 + sum accuracy % / 100)
  float sighting_range = 0.0f;   // m, largest of the installed parts; 0: none
  float recoil_vertical = 0.0f;  // root x (1 + sum recoil % / 100)
  float recoil_horizontal = 0.0f;
  WeaponKind weapon_kind = WeaponKind::kPistol;  // from the root
  FireModeMask fire_modes = 0;                   // from the root
  FireMode default_fire_mode = FireMode::kSemi;  // first listed by the root
  float fire_rate = 0.0f;                        // rpm, from the root
  int magazine_capacity = 0;     // installed magazine; 0 without one
  float muzzle_velocity = 0.0f;  // m/s, barrel x product of velocity factors
  // Every required slot is filled, recursively.
  bool functional = false;
  std::vector<std::string> missing_required;  // MissingRequired()

  friend bool operator==(const WeaponStats&, const WeaponStats&) = default;
};

// Aggregates the stats of `build`:
//   weight      = sum of every part's weight;
//   ergonomics  = clamp(root + sum of the other parts' values, 0, 100);
//   recoil v/h  = root x (1 + sum of the other parts' recoil % / 100), at
//                 least 10 % of the root's value;
//   accuracy    = barrel accuracy_moa x (1 + sum of accuracy % / 100), >= 0;
//   sighting    = max over every installed part;
//   fire modes, fire rate, weapon kind: from the root;
//   capacity    = the installed magazine's;
//   velocity    = barrel muzzle_velocity x product of every part's factor.
// "The barrel" / "the magazine" is the first one in ListSlots order; a
// `complete` root stands in for a missing barrel or magazine with its own
// accuracy_moa, muzzle_velocity and capacity. An empty build yields default
// (all zero, not functional) stats.
WeaponStats ComputeStats(const PartCatalog& catalog, const WeaponBuild& build);

// Stats before and after a change.
struct StatsChange {
  WeaponStats before;
  WeaponStats after;
};

// What installing `sub_build` in slot `path` of `build` would change, worked
// out on a copy (Install's checks and errors apply). `build` is unchanged.
core::Result<StatsChange, BuildError> StatsDelta(const PartCatalog& catalog,
                                                 const WeaponBuild& build,
                                                 std::string_view path,
                                                 const WeaponBuild& sub_build);

}  // namespace blue::weapon::parts

#endif  // BLUE_WEAPON_PARTS_WEAPON_STATS_H_
