#include "weapon/parts/weapon_stats.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"

namespace blue::weapon::parts {
namespace {

constexpr float kMaxErgonomics = 100.0f;
// Recoil never drops below this fraction of the root's value.
constexpr float kMinRecoilFraction = 0.1f;

// Appends every installed part below `build` (not `build` itself) in
// ListSlots order.
void CollectInstalled(const WeaponBuild& build, std::vector<PartId>& out) {
  for (int i = 0; i < build.slot_count(); ++i) {
    if (const WeaponBuild* child = build.installed(i)) {
      out.push_back(child->part());
      CollectInstalled(*child, out);
    }
  }
}

}  // namespace

WeaponStats ComputeStats(const PartCatalog& catalog, const WeaponBuild& build) {
  WeaponStats stats;
  if (build.empty()) {
    return stats;
  }
  const PartDef& root = catalog.part(build.part());
  std::vector<PartId> others;
  CollectInstalled(build, others);

  float ergonomics = root.ergonomics;
  float recoil_percent = 0.0f;
  float accuracy_percent = 0.0f;
  float velocity_factor = root.velocity;
  const PartDef* barrel = nullptr;
  const PartDef* magazine = nullptr;
  stats.weight = root.weight;
  stats.sighting_range = root.sighting_range;
  for (const PartId id : others) {
    const PartDef& part = catalog.part(id);
    stats.weight += part.weight;
    ergonomics += part.ergonomics;
    recoil_percent += part.recoil;
    accuracy_percent += part.accuracy;
    velocity_factor *= part.velocity;
    stats.sighting_range = std::max(stats.sighting_range, part.sighting_range);
    if (barrel == nullptr && part.category == PartCategory::kBarrel) {
      barrel = &part;
    }
    if (magazine == nullptr && part.category == PartCategory::kMagazine) {
      magazine = &part;
    }
  }
  if (root.category == PartCategory::kComplete) {
    barrel = barrel != nullptr ? barrel : &root;
    magazine = magazine != nullptr ? magazine : &root;
  }

  stats.ergonomics = std::clamp(ergonomics, 0.0f, kMaxErgonomics);
  const float recoil_scale =
      std::max(1.0f + recoil_percent / 100.0f, kMinRecoilFraction);
  stats.recoil_vertical = root.recoil_vertical * recoil_scale;
  stats.recoil_horizontal = root.recoil_horizontal * recoil_scale;
  if (barrel != nullptr) {
    stats.accuracy_moa = std::max(
        barrel->accuracy_moa * (1.0f + accuracy_percent / 100.0f), 0.0f);
    stats.muzzle_velocity = barrel->muzzle_velocity * velocity_factor;
  }
  if (magazine != nullptr) {
    stats.magazine_capacity = magazine->capacity;
  }
  stats.weapon_kind = root.weapon_kind;
  for (const FireMode mode : root.fire_modes) {
    stats.fire_modes |= FireModeBit(mode);
  }
  if (!root.fire_modes.empty()) {
    stats.default_fire_mode = root.fire_modes.front();
  }
  stats.fire_rate = root.fire_rate;
  stats.missing_required = MissingRequired(catalog, build);
  stats.functional = stats.missing_required.empty();
  return stats;
}

core::Result<StatsChange, BuildError> StatsDelta(const PartCatalog& catalog,
                                                 const WeaponBuild& build,
                                                 std::string_view path,
                                                 const WeaponBuild& sub_build) {
  using DeltaResult = core::Result<StatsChange, BuildError>;
  WeaponBuild changed = build;
  if (const auto replaced = Install(catalog, changed, path, sub_build);
      !replaced.ok()) {
    return DeltaResult::Error(replaced.error());
  }
  return DeltaResult::Ok(StatsChange{
      .before = ComputeStats(catalog, build),
      .after = ComputeStats(catalog, changed),
  });
}

}  // namespace blue::weapon::parts
