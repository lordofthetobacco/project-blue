#include "weapon/weapon_def.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/log.h"
#include "weapon/weapon_prefab.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

using DefResult = core::Result<std::optional<WeaponDef>, std::string>;

// One registered weapon type: functions rather than values so the table is
// constant-initialised.
struct TypeEntry {
  std::string_view id;
  FirearmSpec (*spec)();
  ViewmodelTuning (*viewmodel)();
  // <= 0: the player's default aim zoom.
  float aim_fov_scale;
  ScopeSpec (*scope)();  // null: no scope
};

constexpr std::array<TypeEntry, 3> kTypes{{
    {"glock17", &Glock17Spec, &Glock17ViewmodelTuning, kPistolAimFovScale,
     nullptr},
    {"m16", &M16Spec, &M16ViewmodelTuning, 0.0f, nullptr},
    {"spr_a3g", &SprA3GSpec, &SprA3GViewmodelTuning, 0.0f, &SprA3GScope},
}};

constexpr std::array<std::string_view, kTypes.size()> kTypeIds{
    kTypes[0].id, kTypes[1].id, kTypes[2].id};

}  // namespace

core::Result<std::optional<FireAnimation>, std::string> ReadFireAnimation(
    const prefab::Prefab& prefab) {
  using FireResult = core::Result<std::optional<FireAnimation>, std::string>;
  const int clip = FindClipIgnoringCase(prefab, kFireClip);
  if (clip < 0) {
    return FireResult::Ok(std::nullopt);
  }
  FireAnimation fire{.hierarchy = prefab.hierarchy,
                     .clip = prefab.clips[static_cast<size_t>(clip)]};
  core::Result<WeaponAnimatorTuning, std::string> tuning =
      AnimatorTuningFromClip(fire.clip);
  if (!tuning.ok()) {
    return FireResult::Error(tuning.error());
  }
  fire.tuning = tuning.value();
  return FireResult::Ok(std::move(fire));
}

std::optional<WeaponType> FindWeaponType(std::string_view id) {
  for (const TypeEntry& entry : kTypes) {
    if (entry.id == id) {
      WeaponType type{
          .id = entry.id, .spec = entry.spec(), .viewmodel = entry.viewmodel()};
      if (entry.aim_fov_scale > 0.0f) {
        type.aim_fov_scale = entry.aim_fov_scale;
      }
      if (entry.scope != nullptr) {
        type.scope = entry.scope();
      }
      return type;
    }
  }
  return std::nullopt;
}

std::span<const std::string_view> WeaponTypeIds() { return kTypeIds; }

core::Result<std::optional<WeaponDef>, std::string> MakeWeaponDef(
    const prefab::Prefab& prefab) {
  const std::optional<std::string_view> tag =
      prefab::FindTag(prefab, kWeaponTag);
  if (!tag) {
    return DefResult::Ok(std::nullopt);
  }
  std::optional<WeaponType> type = FindWeaponType(*tag);
  if (!type) {
    std::string known;
    for (const std::string_view id : WeaponTypeIds()) {
      known += known.empty() ? "" : ", ";
      known += id;
    }
    return DefResult::Error(
        std::format("unknown weapon type '{}' (known: {})", *tag, known));
  }
  core::Result<WeaponGeometry, std::string> geometry =
      GeometryFromSockets(prefab);
  if (!geometry.ok()) {
    return DefResult::Error(geometry.error());
  }
  core::Result<std::optional<FireAnimation>, std::string> fire =
      ReadFireAnimation(prefab);
  if (!fire.ok()) {
    return DefResult::Error(fire.error());
  }
  return DefResult::Ok(WeaponDef{.type = std::move(*type),
                                 .geometry = geometry.value(),
                                 .bounds = prefab.bounds,
                                 .fire = std::move(fire).value(),
                                 .parts = {PartPlacement{}}});
}

WeaponItem MakeFreshWeapon(WeaponDefId id, const WeaponDef& def) {
  float zero = 0.0f;
  if (def.type.scope) {
    zero = ClampZero(*def.type.scope, def.type.scope->start_zero);
  } else if (def.zero_range) {
    zero = parts::ClampZero(*def.zero_range, def.type.viewmodel.zero_distance);
  }
  return {.def = id,
          .firearm = Firearm(def.type.spec),
          .scope_magnification =
              def.type.scope
                  ? ClampMagnification(*def.type.scope,
                                       def.type.scope->start_magnification)
                  : 0.0f,
          .scope_zero = zero,
          .build = def.build,
          .switch_time_scale = def.switch_time_scale};
}

void RefitWeapon(WeaponItem& item, WeaponDefId id, const WeaponDef& def) {
  item.def = id;
  item.build = def.build;
  item.firearm.Refit(def.type.spec);
  item.switch_time_scale = def.switch_time_scale;
  if (def.type.scope) {
    item.scope_magnification =
        ClampMagnification(*def.type.scope, item.scope_magnification);
    item.scope_zero = ClampZero(
        *def.type.scope,
        item.scope_zero > 0.0f ? item.scope_zero : def.type.scope->start_zero);
  } else if (def.zero_range) {
    item.scope_magnification = 0.0f;
    item.scope_zero = parts::ClampZero(*def.zero_range,
                                       item.scope_zero > 0.0f
                                           ? item.scope_zero
                                           : def.type.viewmodel.zero_distance);
  } else {
    item.scope_magnification = 0.0f;
    item.scope_zero = 0.0f;
  }
}

std::optional<WeaponDefId> WeaponLibrary::Register(uint32_t key,
                                                   const prefab::Prefab& prefab,
                                                   std::string_view name) {
  if (const std::optional<WeaponDefId> known = Find(key)) {
    return known;
  }
  if (std::ranges::find(rejected_, key) != rejected_.end()) {
    return std::nullopt;
  }
  DefResult made = MakeWeaponDef(prefab);
  if (!made.ok() || !made.value().has_value()) {
    if (!made.ok()) {
      core::log::Error("{} is not a usable weapon: {}", name, made.error());
    }
    rejected_.push_back(key);
    return std::nullopt;
  }
  const auto id = static_cast<WeaponDefId>(entries_.size());
  made.value()->parts.front().prefab_key = key;
  entries_.push_back(
      {.key = key,
       .def = std::make_unique<WeaponDef>(std::move(*made.value()))});
  core::log::Info("Weapon '{}' from {}", entries_.back().def->type.id, name);
  return id;
}

std::optional<WeaponDefId> WeaponLibrary::Rebuild(uint32_t key,
                                                  const prefab::Prefab& prefab,
                                                  std::string_view name) {
  const std::optional<WeaponDefId> known = Find(key);
  if (!known) {
    std::erase(rejected_, key);
    return Register(key, prefab, name);
  }
  DefResult made = MakeWeaponDef(prefab);
  if (!made.ok()) {
    core::log::Error("Keeping the previous weapon data of {}: {}", name,
                     made.error());
    return known;
  }
  if (!made.value().has_value()) {
    core::log::Error("Keeping the previous weapon data of {}: no '{}' tag",
                     name, kWeaponTag);
    return known;
  }
  made.value()->parts.front().prefab_key = key;
  *entries_[static_cast<size_t>(*known)].def = std::move(*made.value());
  return known;
}

WeaponDefId WeaponLibrary::AddBuild(WeaponDef def) {
  const auto id = static_cast<WeaponDefId>(entries_.size());
  entries_.push_back(
      {.key = std::nullopt, .def = std::make_unique<WeaponDef>()});
  ReplaceBuild(id, std::move(def));
  return id;
}

std::optional<WeaponDefId> WeaponLibrary::FindBuild(
    const parts::WeaponBuild& build, uint32_t root_key) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    const WeaponDef& def = *entries_[i].def;
    if (!entries_[i].key.has_value() && !def.parts.empty() &&
        def.parts.front().prefab_key == root_key && def.build == build) {
      return static_cast<WeaponDefId>(i);
    }
  }
  return std::nullopt;
}

void WeaponLibrary::ReplaceBuild(WeaponDefId id, WeaponDef def) {
  WeaponDef& stored = *entries_[static_cast<size_t>(id)].def;
  stored = std::move(def);
  // A moved string may live elsewhere now: the view follows the storage.
  if (!stored.name.empty()) {
    stored.type.spec.display_name = stored.name;
  }
}

uint32_t WeaponLibrary::prefab_key(WeaponDefId id) const {
  const Entry& entry = entries_[static_cast<size_t>(id)];
  if (entry.key.has_value()) {
    return *entry.key;
  }
  const WeaponDef& def = *entry.def;
  return def.parts.empty() ? 0u : def.parts.front().prefab_key.value_or(0u);
}

std::optional<WeaponDefId> WeaponLibrary::Find(uint32_t key) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].key == key) {
      return static_cast<WeaponDefId>(i);
    }
  }
  return std::nullopt;
}

std::optional<WeaponDefId> WeaponLibrary::FindType(
    std::string_view type_id) const {
  for (size_t i = 0; i < entries_.size(); ++i) {
    if (entries_[i].def->type.id == type_id) {
      return static_cast<WeaponDefId>(i);
    }
  }
  return std::nullopt;
}

}  // namespace blue::weapon
