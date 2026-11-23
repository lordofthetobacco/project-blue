#include "game/weapon_modding.h"

#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/result.h"
#include "game/part_stash.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_stats.h"

namespace blue::game {
namespace {

namespace parts = weapon::parts;

using VoidResult = core::Result<void, std::string>;

enum class Better { kLower, kHigher };

// A stats table row shown as one number.
struct NumericStat {
  std::string_view label;
  float (*get)(const parts::WeaponStats&);
  int decimals;
  std::string_view unit;  // appended to the value, with its leading space
  Better better;
  // A zero shows as `none_text` and means the weapon lacks the figure.
  std::string_view none_text;  // empty: zero is an ordinary value
};

float Weight(const parts::WeaponStats& s) { return s.weight; }
float Ergonomics(const parts::WeaponStats& s) { return s.ergonomics; }
float Accuracy(const parts::WeaponStats& s) { return s.accuracy_moa; }
float Sighting(const parts::WeaponStats& s) { return s.sighting_range; }
float RecoilV(const parts::WeaponStats& s) { return s.recoil_vertical; }
float RecoilH(const parts::WeaponStats& s) { return s.recoil_horizontal; }
float FireRate(const parts::WeaponStats& s) { return s.fire_rate; }
float Magazine(const parts::WeaponStats& s) {
  return static_cast<float>(s.magazine_capacity);
}
float Velocity(const parts::WeaponStats& s) { return s.muzzle_velocity; }

constexpr NumericStat kBeforeFireModes[] = {
    {"Weight", Weight, 2, " kg", Better::kLower, ""},
    {"Ergonomics", Ergonomics, 0, "", Better::kHigher, ""},
    {"Accuracy", Accuracy, 1, " MOA", Better::kLower, "-"},
    {"Sighting range", Sighting, 0, " m", Better::kHigher, "none"},
    {"Vertical recoil", RecoilV, 0, "", Better::kLower, ""},
    {"Horizontal recoil", RecoilH, 0, "", Better::kLower, ""},
};
constexpr NumericStat kAfterFireModes[] = {
    {"Fire rate", FireRate, 0, " rpm", Better::kHigher, ""},
    {"Magazine", Magazine, 0, " rds", Better::kHigher, "none"},
    {"Muzzle velocity", Velocity, 0, " m/s", Better::kHigher, "-"},
};

// `value` rounded to `decimals` places.
float Round(float value, int decimals) {
  const float scale = std::pow(10.0f, static_cast<float>(decimals));
  return std::round(value * scale) / scale;
}

bool IsNone(const NumericStat& stat, float value) {
  return !stat.none_text.empty() && Round(value, stat.decimals) == 0.0f;
}

std::string FormatValue(const NumericStat& stat, float value) {
  if (IsNone(stat, value)) {
    return std::string(stat.none_text);
  }
  return std::format("{:.{}f}{}", value, stat.decimals, stat.unit);
}

StatLine MakeNumericLine(const NumericStat& stat,
                         const parts::WeaponStats& current,
                         const parts::WeaponStats* preview) {
  const float now = stat.get(current);
  StatLine line{.label = stat.label, .value = FormatValue(stat, now)};
  if (preview == nullptr) {
    return line;
  }
  const float next = stat.get(*preview);
  line.preview = FormatValue(stat, next);
  if (line.preview == line.value) {
    return line;
  }
  const bool now_none = IsNone(stat, now);
  const bool next_none = IsNone(stat, next);
  if (now_none || next_none) {
    line.change = next_none ? StatChange::kWorse : StatChange::kBetter;
    return line;
  }
  const float difference =
      Round(next, stat.decimals) - Round(now, stat.decimals);
  line.delta = std::format("{:+.{}f}", difference, stat.decimals);
  const bool lower = difference < 0.0f;
  line.change = lower == (stat.better == Better::kLower) ? StatChange::kBetter
                                                         : StatChange::kWorse;
  return line;
}

std::string FormatFireModes(const parts::WeaponStats& stats) {
  if (stats.fire_modes == 0) {
    return "-";
  }
  // The default mode first, then the others in enum order.
  std::string text(parts::ToString(stats.default_fire_mode));
  for (int i = 0; i < weapon::kFireModeCount; ++i) {
    const auto mode = static_cast<weapon::FireMode>(i);
    if (mode != stats.default_fire_mode &&
        (stats.fire_modes & weapon::FireModeBit(mode)) != 0) {
      text += ", ";
      text += parts::ToString(mode);
    }
  }
  return text;
}

// Number of parts in `build`, its top part included.
int CountParts(const parts::WeaponBuild& build) {
  if (build.empty()) {
    return 0;
  }
  int count = 1;
  for (int slot = 0; slot < build.slot_count(); ++slot) {
    if (const parts::WeaponBuild* child = build.installed(slot)) {
      count += CountParts(*child);
    }
  }
  return count;
}

// Appends `item` to `text` with the list's two-space separator.
void AppendItem(std::string& text, const std::string& item) {
  if (!text.empty()) {
    text += "  ";
  }
  text += item;
}

}  // namespace

std::vector<StatLine> MakeStatLines(const parts::WeaponStats& current,
                                    const parts::WeaponStats* preview) {
  std::vector<StatLine> lines;
  lines.reserve(std::size(kBeforeFireModes) + 1 + std::size(kAfterFireModes));
  for (const NumericStat& stat : kBeforeFireModes) {
    lines.push_back(MakeNumericLine(stat, current, preview));
  }
  StatLine modes{.label = "Fire modes", .value = FormatFireModes(current)};
  if (preview != nullptr) {
    modes.preview = FormatFireModes(*preview);
    if (modes.preview != modes.value) {
      modes.change = StatChange::kDifferent;
    }
  }
  lines.push_back(std::move(modes));
  for (const NumericStat& stat : kAfterFireModes) {
    lines.push_back(MakeNumericLine(stat, current, preview));
  }
  return lines;
}

std::string PartKeyNumbers(const parts::PartDef& part) {
  std::string text;
  if (part.weight != 0.0f) {
    AppendItem(text, std::format("{:.2f} kg", part.weight));
  }
  if (part.ergonomics != 0.0f) {
    AppendItem(text, std::format("ergo {:+.0f}", part.ergonomics));
  }
  if (part.recoil != 0.0f) {
    AppendItem(text, std::format("recoil {:+.1f}%", part.recoil));
  }
  if (part.accuracy != 0.0f) {
    AppendItem(text, std::format("acc {:+.1f}%", part.accuracy));
  }
  if (part.accuracy_moa != 0.0f) {
    AppendItem(text, std::format("{:.1f} MOA", part.accuracy_moa));
  }
  if (part.velocity != 1.0f) {
    AppendItem(text, std::format("vel x{:.2f}", part.velocity));
  }
  if (part.capacity != 0) {
    AppendItem(text, std::format("{} rds", part.capacity));
  }
  if (part.sighting_range != 0.0f) {
    AppendItem(text, std::format("{:.0f} m", part.sighting_range));
  }
  return text;
}

WeaponModding::WeaponModding(const parts::PartCatalog& catalog)
    : catalog_(&catalog) {}

void WeaponModding::SetBuild(const parts::WeaponBuild& build) {
  if (build == build_ && (!rows_.empty() || build.empty())) {
    return;
  }
  build_ = build;
  Refresh();
}

void WeaponModding::Select(int row) {
  selected_ = row >= 0 && row < static_cast<int>(rows_.size()) ? row : -1;
}

bool WeaponModding::CanRemove() const {
  const SlotRow* row = selected_row();
  return row != nullptr && row->installed.valid();
}

std::vector<StashLine> WeaponModding::Candidates(const PartStash& stash) const {
  std::vector<StashLine> lines;
  const SlotRow* row = selected_row();
  if (row == nullptr) {
    return lines;
  }
  for (const int index : stash.Fitting(*catalog_, build_, row->path)) {
    const parts::WeaponBuild& entry = stash.entry(index);
    const parts::PartDef& part = catalog_->part(entry.part());
    const int extra = CountParts(entry) - 1;
    lines.push_back({
        .stash_index = index,
        .name = extra > 0 ? std::format("{} (+{} part{})", part.name, extra,
                                        extra == 1 ? "" : "s")
                          : part.name,
        .category = parts::ToString(part.category),
        .numbers = PartKeyNumbers(part),
    });
  }
  return lines;
}

std::optional<parts::WeaponStats> WeaponModding::PreviewInstall(
    const PartStash& stash, int stash_index) const {
  const SlotRow* row = selected_row();
  if (row == nullptr || stash_index < 0 || stash_index >= stash.size()) {
    return std::nullopt;
  }
  const core::Result<parts::StatsChange, parts::BuildError> change =
      parts::StatsDelta(*catalog_, build_, row->path, stash.entry(stash_index));
  if (!change.ok()) {
    return std::nullopt;
  }
  return change.value().after;
}

std::optional<parts::WeaponStats> WeaponModding::PreviewRemove() const {
  if (!CanRemove()) {
    return std::nullopt;
  }
  parts::WeaponBuild without = build_;
  if (!parts::Remove(*catalog_, without, selected_row()->path).ok()) {
    return std::nullopt;
  }
  return parts::ComputeStats(*catalog_, without);
}

VoidResult WeaponModding::Install(PartStash& stash, int stash_index,
                                  const ApplyBuild& apply) {
  const SlotRow* row = selected_row();
  if (row == nullptr) {
    return VoidResult::Error("no slot is selected");
  }
  if (stash_index < 0 || stash_index >= stash.size()) {
    return VoidResult::Error("no such part in the stash");
  }
  parts::WeaponBuild next = build_;
  core::Result<parts::WeaponBuild, parts::BuildError> replaced =
      parts::Install(*catalog_, next, row->path, stash.entry(stash_index));
  if (!replaced.ok()) {
    return VoidResult::Error(replaced.error().message);
  }
  if (!apply(next)) {
    return VoidResult::Error("the weapon cannot be built with that part");
  }
  // Take first: the entry put back must not shift the taken index.
  (void)stash.Take(stash_index);
  stash.PutBack(std::move(replaced).value());
  build_ = std::move(next);
  Refresh();
  return VoidResult::Ok();
}

VoidResult WeaponModding::Remove(PartStash& stash, const ApplyBuild& apply) {
  if (!CanRemove()) {
    return VoidResult::Error("the selected slot is empty");
  }
  parts::WeaponBuild next = build_;
  core::Result<parts::WeaponBuild, parts::BuildError> removed =
      parts::Remove(*catalog_, next, selected_row()->path);
  if (!removed.ok()) {
    return VoidResult::Error(removed.error().message);
  }
  if (!apply(next)) {
    return VoidResult::Error("the weapon cannot be built without that part");
  }
  stash.PutBack(std::move(removed).value());
  build_ = std::move(next);
  Refresh();
  return VoidResult::Ok();
}

void WeaponModding::Refresh() {
  const std::string selected_path =
      selected_row() != nullptr ? selected_row()->path : std::string();
  rows_.clear();
  for (const parts::SlotEntry& entry : parts::ListSlots(*catalog_, build_)) {
    const parts::SlotDef& slot =
        catalog_->part(entry.owner).slots[static_cast<size_t>(entry.slot)];
    rows_.push_back({
        .path = entry.path,
        .depth = entry.depth,
        .name = slot.name,
        .required = slot.required,
        .installed = entry.installed,
        .installed_name = entry.installed.valid()
                              ? catalog_->part(entry.installed).name
                              : std::string(),
    });
  }
  stats_ = parts::ComputeStats(*catalog_, build_);
  selected_ = rows_.empty() ? -1 : 0;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (rows_[i].path == selected_path) {
      selected_ = static_cast<int>(i);
      break;
    }
  }
}

const SlotRow* WeaponModding::selected_row() const {
  return selected_ >= 0 && selected_ < static_cast<int>(rows_.size())
             ? &rows_[static_cast<size_t>(selected_)]
             : nullptr;
}

}  // namespace blue::game
