#include "weapon/parts/part_catalog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/log.h"
#include "weapon/firearm.h"

namespace blue::weapon::parts {
namespace {

struct CategoryName {
  PartCategory category;
  std::string_view name;
};

constexpr std::array<CategoryName, 10> kCategoryNames = {{
    {PartCategory::kLowerReceiver, "lower_receiver"},
    {PartCategory::kSlide, "slide"},
    {PartCategory::kBarrel, "barrel"},
    {PartCategory::kMagazine, "magazine"},
    {PartCategory::kSightRear, "sight_rear"},
    {PartCategory::kSightFront, "sight_front"},
    {PartCategory::kOptic, "optic"},
    {PartCategory::kMuzzle, "muzzle"},
    {PartCategory::kTactical, "tactical"},
    {PartCategory::kComplete, "complete"},
}};

struct FireModeName {
  FireMode mode;
  std::string_view name;
};

constexpr std::array<FireModeName, kFireModeCount> kFireModeNames = {{
    {FireMode::kSemi, "semi"},
    {FireMode::kAuto, "auto"},
    {FireMode::kDoubleAction, "double_action"},
}};

}  // namespace

std::optional<PartCategory> ParsePartCategory(std::string_view name) {
  for (const CategoryName& entry : kCategoryNames) {
    if (entry.name == name) {
      return entry.category;
    }
  }
  return std::nullopt;
}

std::string_view ToString(PartCategory category) {
  for (const CategoryName& entry : kCategoryNames) {
    if (entry.category == category) {
      return entry.name;
    }
  }
  return "?";
}

bool IsRootCategory(PartCategory category) {
  return category == PartCategory::kLowerReceiver ||
         category == PartCategory::kComplete;
}

std::optional<FireMode> ParseFireMode(std::string_view name) {
  for (const FireModeName& entry : kFireModeNames) {
    if (entry.name == name) {
      return entry.mode;
    }
  }
  return std::nullopt;
}

std::string_view ToString(FireMode mode) {
  for (const FireModeName& entry : kFireModeNames) {
    if (entry.mode == mode) {
      return entry.name;
    }
  }
  return "?";
}

bool SlotDef::Accepts(PartId part) const {
  return std::ranges::find(accepted, part) != accepted.end();
}

int PartDef::FindSlot(std::string_view slot_name) const {
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i].name == slot_name) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

PartCatalog::PartCatalog(std::vector<PartDef> parts,
                         std::vector<PresetDef> presets)
    : parts_(std::move(parts)), presets_(std::move(presets)) {
  for (size_t i = 0; i < parts_.size(); ++i) {
    part_index_.emplace(parts_[i].id, static_cast<int>(i));
  }
  for (size_t i = 0; i < presets_.size(); ++i) {
    preset_index_.emplace(presets_[i].id, static_cast<int>(i));
  }
}

std::optional<PartId> PartCatalog::Find(std::string_view id) const {
  const auto found = part_index_.find(id);
  if (found == part_index_.end()) {
    return std::nullopt;
  }
  return PartId{found->second};
}

const PartDef& PartCatalog::part(PartId id) const {
  if (!Contains(id)) {
    core::log::Fatal("PartCatalog::part: invalid part index {}", id.index);
  }
  return parts_[static_cast<size_t>(id.index)];
}

const PresetDef* PartCatalog::FindPreset(std::string_view id) const {
  const auto found = preset_index_.find(id);
  if (found == preset_index_.end()) {
    return nullptr;
  }
  return &presets_[static_cast<size_t>(found->second)];
}

}  // namespace blue::weapon::parts
