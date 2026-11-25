#include "sim/weapon_table.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_assembly.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"
#include "world/core/prefab_id.h"

namespace blue::sim {
namespace {

namespace parts = weapon::parts;

using DefIdResult = core::Result<weapon::WeaponDefId, std::string>;
using PresetResult =
    core::Result<std::optional<weapon::WeaponDefId>, std::string>;
using TableResult = core::Result<WeaponTable, std::string>;

// 64-bit FNV-1a over values fed as little-endian bytes, so every platform
// hashes the same values alike.
class Fnv64 {
 public:
  void Byte(uint8_t byte) {
    hash_ ^= byte;
    hash_ *= 0x100000001b3ULL;
  }
  void U32(uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      Byte(static_cast<uint8_t>(value >> shift));
    }
  }
  void I32(int value) { U32(static_cast<uint32_t>(value)); }
  void F32(float value) { U32(std::bit_cast<uint32_t>(value)); }
  void Bool(bool value) { Byte(value ? 1 : 0); }

  [[nodiscard]] uint64_t value() const { return hash_; }

 private:
  uint64_t hash_ = 0xcbf29ce484222325ULL;
};

// Feeds what `def` means to the simulation into `hash`. Display texts,
// models, sockets and animation are left out: they are cosmetic.
void HashDef(const weapon::WeaponDef& def, Fnv64& hash) {
  const weapon::FirearmSpec& spec = def.type.spec;
  hash.I32(spec.magazine_capacity);
  hash.F32(spec.rounds_per_minute);
  hash.F32(spec.tactical_reload_seconds);
  hash.F32(spec.empty_reload_seconds);
  hash.F32(spec.muzzle_velocity);
  hash.F32(spec.bullet_mass);
  hash.F32(spec.bullet_drag);
  hash.I32(spec.spare_magazines);
  hash.I32(static_cast<int>(spec.fire_mode));
  hash.U32(spec.fire_modes);
  hash.I32(static_cast<int>(spec.kind));
  hash.F32(spec.mass);
  hash.Bool(spec.functional);
  hash.F32(def.accuracy_moa);
  hash.Bool(def.zero_range.has_value());
  if (def.zero_range) {
    hash.F32(def.zero_range->min);
    hash.F32(def.zero_range->max);
    hash.F32(def.zero_range->step);
  }
  hash.Bool(def.type.scope.has_value());
  if (def.type.scope) {
    hash.F32(def.type.scope->min_zero);
    hash.F32(def.type.scope->max_zero);
    hash.F32(def.type.scope->zero_step);
    hash.F32(def.type.scope->start_zero);
  }
  hash.F32(def.switch_time_scale);
  for (int axis = 0; axis < 3; ++axis) {
    hash.F32(def.bounds.min[axis]);
    hash.F32(def.bounds.max[axis]);
  }
}

}  // namespace

uint32_t Fnv1a32(std::string_view text) {
  uint32_t hash = 0x811c9dc5u;
  for (const char c : text) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x01000193u;
  }
  return hash;
}

std::string PresetWeaponName(std::string_view preset_id) {
  return std::format("preset:{}", preset_id);
}

std::string PrefabWeaponName(std::string_view asset_path) {
  std::string name = std::format("prefab:{}", asset_path);
  std::ranges::replace(name, '\\', '/');
  return name;
}

TableResult WeaponTable::Build(std::span<const WeaponTableEntry> entries,
                               const weapon::WeaponLibrary& library) {
  WeaponTable table;
  table.rows_.reserve(entries.size());
  for (const WeaponTableEntry& entry : entries) {
    table.rows_.push_back({.key = KeyOfWeaponName(entry.name),
                           .def = entry.def,
                           .name = entry.name});
  }
  std::ranges::sort(table.rows_, [](const Row& a, const Row& b) {
    return a.key != b.key ? a.key < b.key : a.name < b.name;
  });
  std::vector<Row> unique;
  unique.reserve(table.rows_.size());
  for (Row& row : table.rows_) {
    if (!unique.empty() && unique.back().key == row.key) {
      const Row& kept = unique.back();
      if (kept.name != row.name) {
        return TableResult::Error(
            std::format("weapons '{}' and '{}' have the same key {:#010x}",
                        kept.name, row.name, row.key));
      }
      if (kept.def != row.def) {
        return TableResult::Error(
            std::format("weapon '{}' is listed for two definitions", row.name));
      }
      continue;  // The same entry twice.
    }
    unique.push_back(std::move(row));
  }
  table.rows_ = std::move(unique);

  Fnv64 hash;
  hash.U32(static_cast<uint32_t>(table.rows_.size()));
  for (const Row& row : table.rows_) {
    hash.U32(row.key);
    HashDef(library.def(row.def), hash);
  }
  table.content_hash_ = hash.value();
  return TableResult::Ok(std::move(table));
}

std::optional<weapon::WeaponDefId> WeaponTable::Find(WeaponKey key) const {
  const auto found = std::ranges::lower_bound(rows_, key, {}, &Row::key);
  if (found == rows_.end() || found->key != key) {
    return std::nullopt;
  }
  return found->def;
}

std::optional<WeaponKey> WeaponTable::KeyOf(weapon::WeaponDefId def) const {
  // Rows are sorted by key: the first match is the smallest key.
  for (const Row& row : rows_) {
    if (row.def == def) {
      return row.key;
    }
  }
  return std::nullopt;
}

core::Result<weapon::WeaponDef, std::string> AssembleBuild(
    const parts::PartCatalog& catalog, const parts::WeaponBuild& build,
    world::PrefabSource& prefabs, std::string_view asset_root) {
  const parts::ModelSource models = [&prefabs,
                                     asset_root](std::string_view path) {
    using ModelResult = core::Result<parts::PartModel, std::string>;
    core::Result<world::PrefabId, prefab::PrefabError> id =
        prefabs.Get(core::JoinPath(asset_root, std::string(path)));
    if (!id.ok()) {
      return ModelResult::Error(prefab::ToString(id.error()));
    }
    return ModelResult::Ok({.key = static_cast<uint32_t>(id.value()),
                            .prefab = &prefabs.prefab(id.value())});
  };
  return parts::AssembleWeapon(catalog, build, models);
}

DefIdResult BuildDef(const parts::PartCatalog& catalog,
                     const parts::WeaponBuild& build,
                     world::PrefabSource& prefabs, std::string_view asset_root,
                     weapon::WeaponLibrary& library) {
  if (build.empty()) {
    return DefIdResult::Error("an empty build is no weapon");
  }
  const parts::PartDef& root = catalog.part(build.part());
  if (root.prefab.empty()) {
    return DefIdResult::Error(
        std::format("part '{}' has no model to build a weapon on", root.id));
  }
  core::Result<world::PrefabId, prefab::PrefabError> root_id =
      prefabs.Get(core::JoinPath(asset_root, root.prefab));
  if (!root_id.ok()) {
    return DefIdResult::Error(std::format("part '{}': cannot load {}: {}",
                                          root.id, root.prefab,
                                          prefab::ToString(root_id.error())));
  }
  if (const std::optional<weapon::WeaponDefId> known =
          library.FindBuild(build, static_cast<uint32_t>(root_id.value()))) {
    return DefIdResult::Ok(*known);
  }
  core::Result<weapon::WeaponDef, std::string> def =
      AssembleBuild(catalog, build, prefabs, asset_root);
  if (!def.ok()) {
    return DefIdResult::Error(def.error());
  }
  return DefIdResult::Ok(library.AddBuild(std::move(def).value()));
}

PresetResult PresetDef(const parts::PartCatalog& catalog, std::string_view id,
                       world::PrefabSource& prefabs,
                       std::string_view asset_root,
                       weapon::WeaponLibrary& library) {
  const parts::PresetDef* preset = catalog.FindPreset(id);
  // A whole weapon the catalogue describes without a model (the sniper
  // rifle) stays the single-piece weapon of its prefab.
  if (preset == nullptr || catalog.part(preset->root).prefab.empty()) {
    return PresetResult::Ok(std::nullopt);
  }
  core::Result<parts::WeaponBuild, parts::BuildError> build =
      parts::MakeBuild(catalog, *preset);
  if (!build.ok()) {
    return PresetResult::Error(build.error().message);
  }
  DefIdResult def =
      BuildDef(catalog, build.value(), prefabs, asset_root, library);
  if (!def.ok()) {
    return PresetResult::Error(def.error());
  }
  return PresetResult::Ok(def.value());
}

TableResult BuildWeaponTable(const parts::PartCatalog* catalog,
                             std::span<const std::string> prefab_paths,
                             world::PrefabSource& prefabs,
                             std::string_view asset_root,
                             weapon::WeaponLibrary& library) {
  std::vector<WeaponTableEntry> entries;
  if (catalog != nullptr) {
    for (const parts::PresetDef& preset : catalog->presets()) {
      PresetResult def =
          PresetDef(*catalog, preset.id, prefabs, asset_root, library);
      if (!def.ok()) {
        core::log::Warn("Weapon preset '{}' cannot be built: {}", preset.id,
                        def.error());
        continue;
      }
      if (def.value()) {
        entries.push_back(
            {.name = PresetWeaponName(preset.id), .def = *def.value()});
      }
    }
  }
  for (const std::string& path : prefab_paths) {
    core::Result<world::PrefabId, prefab::PrefabError> id =
        prefabs.Get(core::JoinPath(asset_root, path));
    if (!id.ok()) {
      return TableResult::Error(std::format("weapon prefab {}: {}", path,
                                            prefab::ToString(id.error())));
    }
    const prefab::Prefab& prefab = prefabs.prefab(id.value());
    std::optional<weapon::WeaponDefId> def;
    if (const std::optional<std::string_view> tag =
            prefab::FindTag(prefab, weapon::kWeaponTag);
        tag && catalog != nullptr) {
      PresetResult preset =
          PresetDef(*catalog, *tag, prefabs, asset_root, library);
      if (preset.ok()) {
        def = preset.value();
      }
    }
    if (!def) {
      def = library.Register(static_cast<uint32_t>(id.value()), prefab,
                             prefabs.key(id.value()));
    }
    if (!def) {
      return TableResult::Error(
          std::format("weapon prefab {} is no usable weapon", path));
    }
    entries.push_back({.name = PrefabWeaponName(path), .def = *def});
  }
  return WeaponTable::Build(entries, library);
}

}  // namespace blue::sim
