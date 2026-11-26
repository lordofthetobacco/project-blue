#include "game/weapon_builds.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "sim/weapon_table.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"

namespace blue::game {
namespace {

namespace parts = weapon::parts;

using DefIdResult = core::Result<weapon::WeaponDefId, std::string>;

}  // namespace

WeaponBuilds::WeaponBuilds(world::PrefabSource& prefabs, std::string asset_root)
    : prefabs_(prefabs), asset_root_(std::move(asset_root)) {}

bool WeaponBuilds::LoadCatalog() {
  const std::string directory =
      core::JoinPath(asset_root_, std::string(kCatalogDirectory));
  core::Result<parts::PartCatalog, parts::CatalogError> loaded =
      parts::LoadPartCatalog(directory);
  if (!loaded.ok()) {
    core::log::Error(
        "No weapon part catalogue ({}); only single-piece weapons are "
        "available",
        loaded.error().ToString());
    catalog_.reset();
    return false;
  }
  core::log::Info("Weapon part catalogue: {} parts, {} presets",
                  loaded.value().part_count(), loaded.value().presets().size());
  catalog_ = std::move(loaded).value();
  reported_presets_.clear();
  return true;
}

void WeaponBuilds::SetCatalog(parts::PartCatalog catalog) {
  catalog_ = std::move(catalog);
  reported_presets_.clear();
}

core::Result<weapon::WeaponDefId, std::string> WeaponBuilds::DefFor(
    const parts::WeaponBuild& build, weapon::WeaponLibrary& library) {
  if (!catalog_) {
    return DefIdResult::Error("there is no weapon part catalogue");
  }
  return sim::BuildDef(*catalog_, build, prefabs_, asset_root_, library);
}

std::optional<weapon::WeaponDefId> WeaponBuilds::PresetDef(
    std::string_view id, weapon::WeaponLibrary& library) {
  if (!catalog_) {
    return std::nullopt;
  }
  core::Result<std::optional<weapon::WeaponDefId>, std::string> def =
      sim::PresetDef(*catalog_, id, prefabs_, asset_root_, library);
  if (def.ok()) {
    return def.value();
  }
  // Logged once per preset.
  if (std::ranges::find(reported_presets_, id) == reported_presets_.end()) {
    reported_presets_.emplace_back(id);
    core::log::Warn("Weapon preset '{}' cannot be built: {}", id, def.error());
  }
  return std::nullopt;
}

int WeaponBuilds::RebuildDefs(weapon::WeaponLibrary& library) {
  if (!catalog_) {
    return 0;
  }
  int rebuilt = 0;
  for (size_t i = 0; i < library.size(); ++i) {
    const auto id = static_cast<weapon::WeaponDefId>(i);
    if (!library.is_build(id)) {
      continue;
    }
    core::Result<weapon::WeaponDef, std::string> def = sim::AssembleBuild(
        *catalog_, library.def(id).build, prefabs_, asset_root_);
    if (!def.ok()) {
      core::log::Error("Keeping the previous data of weapon '{}': {}",
                       library.def(id).type.spec.display_name, def.error());
      continue;
    }
    library.ReplaceBuild(id, std::move(def).value());
    ++rebuilt;
  }
  return rebuilt;
}

}  // namespace blue::game
