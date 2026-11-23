#include "game/part_stash.h"

#include <cassert>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"

namespace blue::game {
namespace {

namespace parts = weapon::parts;

// Marks every part of `build` in `held` (indexed by PartId).
void MarkParts(const parts::WeaponBuild& build, std::vector<bool>& held) {
  if (build.empty()) {
    return;
  }
  const auto index = static_cast<size_t>(build.part().index);
  if (index < held.size()) {
    held[index] = true;
  }
  for (int slot = 0; slot < build.slot_count(); ++slot) {
    if (const parts::WeaponBuild* child = build.installed(slot)) {
      MarkParts(*child, held);
    }
  }
}

}  // namespace

void PartStash::Seed(const parts::PartCatalog& catalog,
                     std::span<const parts::WeaponBuild> starting) {
  entries_.clear();
  std::vector<bool> held(static_cast<size_t>(catalog.part_count()), false);
  for (const parts::WeaponBuild& build : starting) {
    MarkParts(build, held);
  }
  for (int i = 0; i < catalog.part_count(); ++i) {
    const parts::PartId id{i};
    const parts::PartDef& part = catalog.part(id);
    if (parts::IsRootCategory(part.category)) {
      continue;
    }
    const bool magazine = part.category == parts::PartCategory::kMagazine;
    if (magazine || !held[static_cast<size_t>(i)]) {
      entries_.emplace_back(catalog, id);
    }
  }
}

void PartStash::PutBack(parts::WeaponBuild entry) {
  if (!entry.empty()) {
    entries_.push_back(std::move(entry));
  }
}

parts::WeaponBuild PartStash::Take(int index) {
  assert(index >= 0 && index < size());
  const auto at = entries_.begin() + index;
  parts::WeaponBuild taken = std::move(*at);
  entries_.erase(at);
  return taken;
}

const parts::WeaponBuild& PartStash::entry(int index) const {
  assert(index >= 0 && index < size());
  return entries_[static_cast<size_t>(index)];
}

std::vector<int> PartStash::Fitting(const parts::PartCatalog& catalog,
                                    const parts::WeaponBuild& build,
                                    std::string_view path) const {
  std::vector<int> fitting;
  for (int i = 0; i < size(); ++i) {
    if (parts::CanInstall(catalog, build, path, entry(i).part()).ok()) {
      fitting.push_back(i);
    }
  }
  return fitting;
}

}  // namespace blue::game
