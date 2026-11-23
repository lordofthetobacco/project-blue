#ifndef BLUE_WEAPON_PARTS_WEAPON_BUILD_H_
#define BLUE_WEAPON_PARTS_WEAPON_BUILD_H_

#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "weapon/parts/part_catalog.h"

// A weapon as a tree of installed parts, and the operations that change it.
// Every operation checks the catalogue's slot filters, so a build made
// through them only ever holds parts its slots accept.
//
// Slot paths name a slot by the slot names from the root down, joined by
// '.': "barrel" is the root's barrel slot, "slide.sight_rear" the rear sight
// slot of the part in the root's slide slot.

namespace blue::weapon::parts {

// Why a build operation was refused.
struct BuildError {
  std::string message;
};

// A part and, per slot it offers, the sub-build installed there. Value type:
// copying copies the whole tree (a few hundred bytes for a pistol); equal
// builds hold the same parts in the same slots. Holds no pointers into the
// catalogue, only PartIds, so it stays valid while the same catalogue is
// used. Not thread-safe for mutation.
class WeaponBuild {
 public:
  // An empty build: no part.
  WeaponBuild() = default;
  // `part` (valid in `catalog`) with every slot empty.
  WeaponBuild(const PartCatalog& catalog, PartId part);
  WeaponBuild(const WeaponBuild&) = default;
  WeaponBuild& operator=(const WeaponBuild&) = default;
  WeaponBuild(WeaponBuild&&) = default;
  WeaponBuild& operator=(WeaponBuild&&) = default;

  [[nodiscard]] bool empty() const { return !part_.valid(); }
  // The part at the top of this (sub-)build; invalid when empty().
  [[nodiscard]] PartId part() const { return part_; }
  // Number of slots part() offers (0 when empty).
  [[nodiscard]] int slot_count() const {
    return static_cast<int>(slots_.size());
  }
  // The sub-build in slot `slot` (0 <= slot < slot_count()), or nullptr when
  // that slot is empty.
  [[nodiscard]] const WeaponBuild* installed(int slot) const;

  friend bool operator==(const WeaponBuild&, const WeaponBuild&) = default;

 private:
  friend core::Result<WeaponBuild, BuildError> Install(
      const PartCatalog& catalog, WeaponBuild& build, std::string_view path,
      WeaponBuild sub_build);
  friend core::Result<WeaponBuild, BuildError> Remove(
      const PartCatalog& catalog, WeaponBuild& build, std::string_view path);

  PartId part_;
  // One entry per slot of part_, in the catalogue's slot order; an empty
  // build marks an empty slot.
  std::vector<WeaponBuild> slots_;
};

// Builds `preset`, installing its entries parents first.
core::Result<WeaponBuild, BuildError> MakeBuild(const PartCatalog& catalog,
                                                const PresetDef& preset);

// Checks that slot `path` exists in `build` (its parent part is installed)
// and that its filter accepts `part`. The slot may be occupied.
core::Result<void, BuildError> CanInstall(const PartCatalog& catalog,
                                          const WeaponBuild& build,
                                          std::string_view path, PartId part);

// Installs `sub_build` (non-empty, with its own children) in slot `path`
// after the CanInstall checks. Returns what the slot held before (an empty
// build when it was empty). `build` is unchanged on error.
core::Result<WeaponBuild, BuildError> Install(const PartCatalog& catalog,
                                              WeaponBuild& build,
                                              std::string_view path,
                                              WeaponBuild sub_build);

// Takes the sub-build out of slot `path` and returns it with its own
// children. Fails when the slot does not exist or is empty.
core::Result<WeaponBuild, BuildError> Remove(const PartCatalog& catalog,
                                             WeaponBuild& build,
                                             std::string_view path);

// One slot of a build, for a slot-tree UI.
struct SlotEntry {
  std::string path;
  int depth = 0;     // 0: a slot of the root
  PartId owner;      // the part offering the slot
  int slot = 0;      // index into the owner's slots
  PartId installed;  // invalid when empty
};

// Every slot of `build` depth first: a slot, then the slots of the part in
// it, then the owner's next slot. Empty for an empty build.
std::vector<SlotEntry> ListSlots(const PartCatalog& catalog,
                                 const WeaponBuild& build);

// Paths of the required slots that are empty, in ListSlots order. Slots
// below an empty slot are not listed. An empty build reports nothing.
std::vector<std::string> MissingRequired(const PartCatalog& catalog,
                                         const WeaponBuild& build);

}  // namespace blue::weapon::parts

#endif  // BLUE_WEAPON_PARTS_WEAPON_BUILD_H_
