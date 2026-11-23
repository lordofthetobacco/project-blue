#include "game/weapon_builds.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "core/path.h"
#include "core/result.h"
#include "prefab/prefab_file.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/parts/weapon_assembly.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"

namespace blue::game {
namespace {

namespace parts = weapon::parts;

using DefIdResult = core::Result<weapon::WeaponDefId, std::string>;

}  // namespace

WeaponBuilds::WeaponBuilds(world::PrefabCache& cache, std::string asset_root)
    : cache_(cache), asset_root_(std::move(asset_root)) {}

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

core::Result<weapon::WeaponDef, std::string> WeaponBuilds::Assemble(
    const parts::WeaponBuild& build) {
  const parts::ModelSource models = [this](std::string_view path) {
    using ModelResult = core::Result<parts::PartModel, std::string>;
    core::Result<world::PrefabId, prefab::PrefabError> id =
        cache_.Get(core::JoinPath(asset_root_, std::string(path)));
    if (!id.ok()) {
      return ModelResult::Error(prefab::ToString(id.error()));
    }
    return ModelResult::Ok({.key = static_cast<uint32_t>(id.value()),
                            .prefab = &cache_.prefab(id.value())});
  };
  return parts::AssembleWeapon(*catalog_, build, models);
}

core::Result<weapon::WeaponDefId, std::string> WeaponBuilds::DefFor(
    const parts::WeaponBuild& build, weapon::WeaponLibrary& library) {
  if (!catalog_) {
    return DefIdResult::Error("there is no weapon part catalogue");
  }
  if (build.empty()) {
    return DefIdResult::Error("an empty build is no weapon");
  }
  const parts::PartDef& root = catalog_->part(build.part());
  if (root.prefab.empty()) {
    return DefIdResult::Error(
        std::format("part '{}' has no model to build a weapon on", root.id));
  }
  core::Result<world::PrefabId, prefab::PrefabError> root_id =
      cache_.Get(core::JoinPath(asset_root_, root.prefab));
  if (!root_id.ok()) {
    return DefIdResult::Error(std::format("part '{}': cannot load {}: {}",
                                          root.id, root.prefab,
                                          prefab::ToString(root_id.error())));
  }
  if (const std::optional<weapon::WeaponDefId> known =
          library.FindBuild(build, static_cast<uint32_t>(root_id.value()))) {
    return DefIdResult::Ok(*known);
  }
  core::Result<weapon::WeaponDef, std::string> def = Assemble(build);
  if (!def.ok()) {
    return DefIdResult::Error(def.error());
  }
  return DefIdResult::Ok(library.AddBuild(std::move(def).value()));
}

std::optional<weapon::WeaponDefId> WeaponBuilds::PresetDef(
    std::string_view id, weapon::WeaponLibrary& library) {
  if (!catalog_) {
    return std::nullopt;
  }
  const parts::PresetDef* preset = catalog_->FindPreset(id);
  // A whole weapon the catalogue describes without a model (the sniper
  // rifle) stays the single-piece weapon of its prefab.
  if (preset == nullptr || catalog_->part(preset->root).prefab.empty()) {
    return std::nullopt;
  }
  const auto report = [this, id](const std::string& message) {
    if (std::ranges::find(reported_presets_, id) == reported_presets_.end()) {
      reported_presets_.emplace_back(id);
      core::log::Warn("Weapon preset '{}' cannot be built: {}", id, message);
    }
  };
  core::Result<parts::WeaponBuild, parts::BuildError> build =
      parts::MakeBuild(*catalog_, *preset);
  if (!build.ok()) {
    report(build.error().message);
    return std::nullopt;
  }
  DefIdResult def = DefFor(build.value(), library);
  if (!def.ok()) {
    report(def.error());
    return std::nullopt;
  }
  return def.value();
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
    core::Result<weapon::WeaponDef, std::string> def =
        Assemble(library.def(id).build);
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
