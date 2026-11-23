#ifndef BLUE_GAME_WEAPON_MODDING_H_
#define BLUE_GAME_WEAPON_MODDING_H_

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "game/part_stash.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_stats.h"

// The logic of the weapon inspect / modding screen, without any UI: the
// slot tree of the weapon in hand, what the stash offers for a slot, the
// stats with the change a candidate would make, and installing / removing
// parts. The screen (ui/weapon_inspect_screen.h) only shows what this
// prepares and reports clicks.

namespace blue::game {

// One slot of the weapon in hand, as a row of the slot tree.
struct SlotRow {
  std::string path;  // slot path ("slide.sight_rear")
  int depth = 0;     // 0: a slot of the root part
  std::string name;  // the slot's own name ("sight_rear")
  bool required = false;
  weapon::parts::PartId installed;  // invalid when empty
  std::string installed_name;       // empty when empty
};

// How a stat changes between the current build and a preview.
enum class StatChange {
  kNone,       // no preview, or the shown value stays the same
  kBetter,     // green
  kWorse,      // red
  kDifferent,  // changed, neither better nor worse (fire modes)
};

// One row of the stats table.
struct StatLine {
  std::string_view label;  // "Weight", ... (static text)
  std::string value;       // the current value, formatted
  std::string preview;     // the previewed value; empty without a preview
  std::string delta;       // signed difference ("-0.20"); empty if none
  StatChange change = StatChange::kNone;
};

// Returns the stats table of `current` (Weight, Ergonomics, Accuracy,
// Sighting range, Vertical recoil, Horizontal recoil, Fire modes, Fire
// rate, Magazine, Muzzle velocity, in that order) and, with `preview`, the
// value each row would have and whether that is better: lower is better
// for weight, accuracy (MOA) and both recoils, higher for the others; a
// figure that goes to "none" (no barrel, no sights, no magazine) is worse.
// Values that format the same count as unchanged.
[[nodiscard]] std::vector<StatLine> MakeStatLines(
    const weapon::parts::WeaponStats& current,
    const weapon::parts::WeaponStats* preview);

// Returns the key numbers of `part` for the stash list, the non-zero ones
// of: weight, ergonomics, recoil %, accuracy %, MOA, velocity, capacity,
// sighting range; e.g. "0.20 kg  ergo -12  recoil -15%  acc -5%  vel x0.95".
[[nodiscard]] std::string PartKeyNumbers(const weapon::parts::PartDef& part);

// One stash entry that fits the selected slot.
struct StashLine {
  int stash_index = 0;        // PartStash index
  std::string name;           // the part's name, "+N parts" when it holds some
  std::string_view category;  // catalogue spelling
  std::string numbers;        // PartKeyNumbers of the top part
};

// Makes the weapon in hand a weapon of the given build; false when it
// cannot (App::ChangeHeldWeaponBuild).
using ApplyBuild = std::function<bool(const weapon::parts::WeaponBuild&)>;

// A modding session on the weapon in hand: its build, slot rows, stats and
// the selected slot. Borrows the catalogue, which must outlive it and not
// change. The root part is no slot, so it can neither be removed nor
// replaced. Copyable. Not thread-safe.
class WeaponModding {
 public:
  explicit WeaponModding(const weapon::parts::PartCatalog& catalog);

  // Shows `build` (the weapon in hand). Keeps the selected slot when the
  // new build still has it, else selects the first slot. Cheap when the
  // build is the one shown.
  void SetBuild(const weapon::parts::WeaponBuild& build);
  [[nodiscard]] const weapon::parts::WeaponBuild& build() const {
    return build_;
  }
  [[nodiscard]] const weapon::parts::WeaponStats& stats() const {
    return stats_;
  }
  [[nodiscard]] std::span<const SlotRow> rows() const { return rows_; }

  // The selected row, -1 when there is none (a build without slots).
  [[nodiscard]] int selected() const { return selected_; }
  // Selects row `row`; an index out of range selects nothing.
  void Select(int row);
  // True when the selected slot holds a part.
  [[nodiscard]] bool CanRemove() const;

  // The entries of `stash` that fit the selected slot, in stash order;
  // empty without a selection.
  [[nodiscard]] std::vector<StashLine> Candidates(const PartStash& stash) const;

  // The stats with stash entry `stash_index` installed in the selected slot
  // (what it holds goes back to the stash); nullopt when it does not fit.
  [[nodiscard]] std::optional<weapon::parts::WeaponStats> PreviewInstall(
      const PartStash& stash, int stash_index) const;
  // The stats with the selected slot emptied; nullopt when it is empty.
  [[nodiscard]] std::optional<weapon::parts::WeaponStats> PreviewRemove() const;

  // Installs stash entry `stash_index` in the selected slot: the entry
  // leaves the stash, what the slot held goes back to it, and `apply` gets
  // the new build. When the entry does not fit or `apply` refuses, the
  // stash and the build are left exactly as they were (error says why).
  core::Result<void, std::string> Install(PartStash& stash, int stash_index,
                                          const ApplyBuild& apply);
  // Takes the sub-assembly out of the selected slot into the stash, after
  // `apply` accepted the build without it; nothing changes otherwise.
  core::Result<void, std::string> Remove(PartStash& stash,
                                         const ApplyBuild& apply);

 private:
  // Rebuilds rows_ and stats_ from build_, keeping the selection by path.
  void Refresh();
  [[nodiscard]] const SlotRow* selected_row() const;

  const weapon::parts::PartCatalog* catalog_;
  weapon::parts::WeaponBuild build_;
  weapon::parts::WeaponStats stats_;
  std::vector<SlotRow> rows_;
  int selected_ = -1;
};

}  // namespace blue::game

#endif  // BLUE_GAME_WEAPON_MODDING_H_
