#include "weapon/parts/weapon_build.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"
#include "weapon/parts/part_catalog.h"

namespace blue::weapon::parts {
namespace {

using SlotIndices = std::vector<int>;
using PathResult = core::Result<SlotIndices, BuildError>;

PathResult PathError(std::string message) {
  return PathResult::Error(BuildError{std::move(message)});
}

// Returns the slot indices that lead from the root of `build` to the slot
// named by `path`; every slot but the last must hold a part.
PathResult ResolvePath(const PartCatalog& catalog, const WeaponBuild& build,
                       std::string_view path) {
  if (build.empty()) {
    return PathError("the build is empty");
  }
  if (path.empty()) {
    return PathError("empty slot path");
  }
  SlotIndices indices;
  const WeaponBuild* node = &build;
  size_t start = 0;
  while (true) {
    const size_t dot = path.find('.', start);
    const bool last = dot == std::string_view::npos;
    const std::string_view name =
        path.substr(start, last ? std::string_view::npos : dot - start);
    if (name.empty()) {
      return PathError(std::format("bad slot path '{}'", path));
    }
    const PartDef& owner = catalog.part(node->part());
    const int slot = owner.FindSlot(name);
    if (slot < 0) {
      return PathError(std::format("part '{}' has no slot '{}' (path '{}')",
                                   owner.id, name, path));
    }
    indices.push_back(slot);
    if (last) {
      return PathResult::Ok(std::move(indices));
    }
    node = node->installed(slot);
    if (node == nullptr) {
      return PathError(std::format("slot '{}' is empty (path '{}')",
                                   path.substr(0, dot), path));
    }
    start = dot + 1;
  }
}

// Returns the part that owns the last slot of `indices`.
PartId OwnerOf(const WeaponBuild& build, const SlotIndices& indices) {
  const WeaponBuild* node = &build;
  for (size_t i = 0; i + 1 < indices.size(); ++i) {
    node = node->installed(indices[i]);
  }
  return node->part();
}

int Depth(std::string_view path) {
  return static_cast<int>(std::ranges::count(path, '.'));
}

void AppendSlots(const PartCatalog& catalog, const WeaponBuild& build,
                 const std::string& prefix, int depth,
                 std::vector<SlotEntry>& out) {
  const PartDef& owner = catalog.part(build.part());
  for (int i = 0; i < build.slot_count(); ++i) {
    const WeaponBuild* child = build.installed(i);
    SlotEntry entry{
        .path = prefix + owner.slots[static_cast<size_t>(i)].name,
        .depth = depth,
        .owner = build.part(),
        .slot = i,
        .installed = child != nullptr ? child->part() : PartId{},
    };
    const std::string child_prefix = entry.path + ".";
    out.push_back(std::move(entry));
    if (child != nullptr) {
      AppendSlots(catalog, *child, child_prefix, depth + 1, out);
    }
  }
}

}  // namespace

WeaponBuild::WeaponBuild(const PartCatalog& catalog, PartId part)
    : part_(part), slots_(catalog.part(part).slots.size()) {}

const WeaponBuild* WeaponBuild::installed(int slot) const {
  if (slot < 0 || slot >= slot_count()) {
    return nullptr;
  }
  const WeaponBuild& child = slots_[static_cast<size_t>(slot)];
  return child.empty() ? nullptr : &child;
}

core::Result<WeaponBuild, BuildError> MakeBuild(const PartCatalog& catalog,
                                                const PresetDef& preset) {
  using BuildResult = core::Result<WeaponBuild, BuildError>;
  if (!catalog.Contains(preset.root) ||
      !IsRootCategory(catalog.part(preset.root).category)) {
    return BuildResult::Error(
        {std::format("preset '{}': the root is not a lower_receiver or "
                     "complete part",
                     preset.id)});
  }
  WeaponBuild build(catalog, preset.root);
  // Parents before children; entries of equal depth keep the file order.
  std::vector<const PresetEntry*> order;
  for (const PresetEntry& entry : preset.entries) {
    order.push_back(&entry);
  }
  std::ranges::stable_sort(
      order, {}, [](const PresetEntry* entry) { return Depth(entry->path); });
  for (const PresetEntry* entry : order) {
    if (!catalog.Contains(entry->part)) {
      return BuildResult::Error({std::format(
          "preset '{}': unknown part for '{}'", preset.id, entry->path)});
    }
    auto replaced =
        Install(catalog, build, entry->path, WeaponBuild(catalog, entry->part));
    if (!replaced.ok()) {
      return BuildResult::Error({std::format("preset '{}': {}", preset.id,
                                             replaced.error().message)});
    }
    if (!replaced.value().empty()) {
      return BuildResult::Error({std::format(
          "preset '{}': slot '{}' is set twice", preset.id, entry->path)});
    }
  }
  return BuildResult::Ok(std::move(build));
}

core::Result<void, BuildError> CanInstall(const PartCatalog& catalog,
                                          const WeaponBuild& build,
                                          std::string_view path, PartId part) {
  using CheckResult = core::Result<void, BuildError>;
  const PathResult indices = ResolvePath(catalog, build, path);
  if (!indices.ok()) {
    return CheckResult::Error(indices.error());
  }
  if (!catalog.Contains(part)) {
    return CheckResult::Error({"unknown part"});
  }
  const PartDef& owner = catalog.part(OwnerOf(build, indices.value()));
  const SlotDef& slot =
      owner.slots[static_cast<size_t>(indices.value().back())];
  if (!slot.Accepts(part)) {
    return CheckResult::Error({std::format(
        "slot '{}' does not accept part '{}'", path, catalog.part(part).id)});
  }
  return CheckResult::Ok();
}

core::Result<WeaponBuild, BuildError> Install(const PartCatalog& catalog,
                                              WeaponBuild& build,
                                              std::string_view path,
                                              WeaponBuild sub_build) {
  using InstallResult = core::Result<WeaponBuild, BuildError>;
  if (sub_build.empty()) {
    return InstallResult::Error({"nothing to install"});
  }
  if (const auto check = CanInstall(catalog, build, path, sub_build.part());
      !check.ok()) {
    return InstallResult::Error(check.error());
  }
  const SlotIndices indices = ResolvePath(catalog, build, path).value();
  WeaponBuild* node = &build;
  for (size_t i = 0; i + 1 < indices.size(); ++i) {
    node = &node->slots_[static_cast<size_t>(indices[i])];
  }
  WeaponBuild& slot = node->slots_[static_cast<size_t>(indices.back())];
  return InstallResult::Ok(std::exchange(slot, std::move(sub_build)));
}

core::Result<WeaponBuild, BuildError> Remove(const PartCatalog& catalog,
                                             WeaponBuild& build,
                                             std::string_view path) {
  using RemoveResult = core::Result<WeaponBuild, BuildError>;
  const PathResult indices = ResolvePath(catalog, build, path);
  if (!indices.ok()) {
    return RemoveResult::Error(indices.error());
  }
  WeaponBuild* node = &build;
  for (size_t i = 0; i + 1 < indices.value().size(); ++i) {
    node = &node->slots_[static_cast<size_t>(indices.value()[i])];
  }
  WeaponBuild& slot = node->slots_[static_cast<size_t>(indices.value().back())];
  if (slot.empty()) {
    return RemoveResult::Error({std::format("slot '{}' is empty", path)});
  }
  return RemoveResult::Ok(std::exchange(slot, WeaponBuild()));
}

std::vector<SlotEntry> ListSlots(const PartCatalog& catalog,
                                 const WeaponBuild& build) {
  std::vector<SlotEntry> slots;
  if (!build.empty()) {
    AppendSlots(catalog, build, "", 0, slots);
  }
  return slots;
}

std::vector<std::string> MissingRequired(const PartCatalog& catalog,
                                         const WeaponBuild& build) {
  std::vector<std::string> missing;
  for (const SlotEntry& entry : ListSlots(catalog, build)) {
    const SlotDef& slot =
        catalog.part(entry.owner).slots[static_cast<size_t>(entry.slot)];
    if (slot.required && !entry.installed.valid()) {
      missing.push_back(entry.path);
    }
  }
  return missing;
}

}  // namespace blue::weapon::parts
