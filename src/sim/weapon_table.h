#ifndef BLUE_SIM_WEAPON_TABLE_H_
#define BLUE_SIM_WEAPON_TABLE_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/result.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "world/core/prefab_source.h"

// Stable weapon identity across processes, and the rule that decides which
// weapons exist. weapon::WeaponDefId is a registration-order index that
// differs between the client and the dedicated server; a WeaponKey names a
// weapon by where it comes from, so both sides agree on it.

namespace blue::sim {

// 32-bit FNV-1a of "preset:<id>" or "prefab:<asset-relative path with
// forward slashes>".
using WeaponKey = uint32_t;

// Returns the 32-bit FNV-1a hash of `text`.
[[nodiscard]] uint32_t Fnv1a32(std::string_view text);

// Returns the stable name of catalogue preset `preset_id`: "preset:<id>".
[[nodiscard]] std::string PresetWeaponName(std::string_view preset_id);
// Returns the stable name of the single-piece weapon at `asset_path`
// (relative to the asset root): "prefab:<path>", backslashes turned into
// forward slashes.
[[nodiscard]] std::string PrefabWeaponName(std::string_view asset_path);

// Returns the key of a stable weapon name (PresetWeaponName(),
// PrefabWeaponName()).
[[nodiscard]] inline WeaponKey KeyOfWeaponName(std::string_view name) {
  return Fnv1a32(name);
}

// One weapon of a table: its stable name and its definition in the caller's
// library.
struct WeaponTableEntry {
  std::string name;
  weapon::WeaponDefId def{};
};

// Maps WeaponKeys to the definitions of one process's WeaponLibrary and
// back, and fingerprints what the definitions mean to the simulation.
// Built once after the weapons are registered; immutable afterwards.
// Copyable and movable.
class WeaponTable {
 public:
  // One row, ordered by key.
  struct Row {
    WeaponKey key = 0;
    weapon::WeaponDefId def{};
    std::string name;
  };

  // An empty table.
  WeaponTable() = default;

  // Builds the table of `entries`, whose definitions come from `library`.
  // Fails, naming both, when two different names hash to the same key, or
  // when one name is listed for two different definitions. Several names
  // may share a definition (a preset and the prefab it replaces).
  [[nodiscard]] static core::Result<WeaponTable, std::string> Build(
      std::span<const WeaponTableEntry> entries,
      const weapon::WeaponLibrary& library);

  // Returns the definition of `key`, or nullopt.
  [[nodiscard]] std::optional<weapon::WeaponDefId> Find(WeaponKey key) const;
  // Returns the key of `def`: the smallest key naming it (the same choice
  // in every process), or nullopt when the table does not know it.
  [[nodiscard]] std::optional<WeaponKey> KeyOf(weapon::WeaponDefId def) const;

  // 64-bit FNV-1a over the keys in order and, per key, the definition's
  // fields that affect the simulation (firearm spec except the display name,
  // accuracy, zero range, scope zero range, switch time, dropped bounds).
  // Client and server compare it in the handshake.
  [[nodiscard]] uint64_t content_hash() const { return content_hash_; }

  [[nodiscard]] std::span<const Row> rows() const { return rows_; }
  [[nodiscard]] size_t size() const { return rows_.size(); }

 private:
  std::vector<Row> rows_;  // sorted by key, keys unique
  uint64_t content_hash_ = 0;
};

// The build rule shared by game::WeaponBuilds and the headless simulation.
// Part prefabs are loaded through `prefabs` from paths relative to
// `asset_root`.

// Assembles the definition of `build` (not added to any library); an error
// message when it cannot be assembled.
[[nodiscard]] core::Result<weapon::WeaponDef, std::string> AssembleBuild(
    const weapon::parts::PartCatalog& catalog,
    const weapon::parts::WeaponBuild& build, world::PrefabSource& prefabs,
    std::string_view asset_root);

// Returns the definition of `build` from `library`, assembling and adding it
// the first time (WeaponLibrary::FindBuild); an error message (not logged)
// when it cannot be assembled.
[[nodiscard]] core::Result<weapon::WeaponDefId, std::string> BuildDef(
    const weapon::parts::PartCatalog& catalog,
    const weapon::parts::WeaponBuild& build, world::PrefabSource& prefabs,
    std::string_view asset_root, weapon::WeaponLibrary& library);

// Returns the definition of preset `id` like BuildDef(): nullopt when there
// is no such preset or its root part has no model (a single-piece weapon
// known by its prefab only), an error message when it cannot be built.
[[nodiscard]] core::Result<std::optional<weapon::WeaponDefId>, std::string>
PresetDef(const weapon::parts::PartCatalog& catalog, std::string_view id,
          world::PrefabSource& prefabs, std::string_view asset_root,
          weapon::WeaponLibrary& library);

// Registers every weapon a networked game knows into `library` and returns
// their table: each preset of `catalog` (if any) that has a model, in
// catalogue order (one that cannot be built is logged and left out, which
// changes the content hash), then each single-piece weapon prefab of
// `prefab_paths` (relative to `asset_root`; a prefab whose `weapon` tag
// names a preset with a model is that preset's definition, as in the game).
// Fails when a listed prefab cannot be loaded or is no weapon, or keys
// collide.
[[nodiscard]] core::Result<WeaponTable, std::string> BuildWeaponTable(
    const weapon::parts::PartCatalog* catalog,
    std::span<const std::string> prefab_paths, world::PrefabSource& prefabs,
    std::string_view asset_root, weapon::WeaponLibrary& library);

}  // namespace blue::sim

#endif  // BLUE_SIM_WEAPON_TABLE_H_
