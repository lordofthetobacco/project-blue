#ifndef BLUE_GAME_PART_STASH_H_
#define BLUE_GAME_PART_STASH_H_

#include <span>
#include <string_view>
#include <vector>

#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"

// The player's loose weapon parts, for the inspect / modding screen.

namespace blue::game {

// A flat list of loose sub-assemblies: each entry is a WeaponBuild whose top
// part is the loose part, with whatever is still installed on it (a removed
// slide keeps its sights). Entries keep their order; one put back goes to
// the end. Holds no pointers into the catalogue. Copyable value type. Not
// thread-safe.
class PartStash {
 public:
  PartStash() = default;

  // Empties the stash and fills it with the starter set: one bare entry of
  // every non-root part of `catalog` that none of `starting` holds, plus one
  // of every magazine, in catalogue order.
  void Seed(const weapon::parts::PartCatalog& catalog,
            std::span<const weapon::parts::WeaponBuild> starting);
  void Clear() { entries_.clear(); }

  // Puts `entry` back (appended). An empty build is ignored.
  void PutBack(weapon::parts::WeaponBuild entry);
  // Takes entry `index` out (0 <= index < size()) and returns it.
  [[nodiscard]] weapon::parts::WeaponBuild Take(int index);

  [[nodiscard]] int size() const { return static_cast<int>(entries_.size()); }
  [[nodiscard]] bool empty() const { return entries_.empty(); }
  // 0 <= index < size().
  [[nodiscard]] const weapon::parts::WeaponBuild& entry(int index) const;

  // Indices of the entries that could go into slot `path` of `build`
  // (CanInstall of their top part), in stash order.
  [[nodiscard]] std::vector<int> Fitting(
      const weapon::parts::PartCatalog& catalog,
      const weapon::parts::WeaponBuild& build, std::string_view path) const;

 private:
  std::vector<weapon::parts::WeaponBuild> entries_;
};

}  // namespace blue::game

#endif  // BLUE_GAME_PART_STASH_H_
