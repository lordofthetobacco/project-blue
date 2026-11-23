#include "weapon/parts/part_catalog_file.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/path.h"
#include "core/result.h"
#include "core/text.h"
#include "platform/file_system.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"

namespace blue::weapon::parts {
namespace {

using FileResult = core::Result<PartCatalogFile, CatalogError>;
using CatalogResult = core::Result<PartCatalog, CatalogError>;
// nullopt: fine.
using MaybeError = std::optional<CatalogError>;

constexpr size_t kMaxCatalogFileSize = 4 * 1024 * 1024;
constexpr std::string_view kCatalogExtension = ".cfg";
// Fresh copies `blue-pack samples` writes next to a user-edited file.
constexpr std::string_view kGeneratedExtension = ".generated.cfg";
constexpr std::string_view kSlotPrefix = "slot.";
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

bool IsIdentifier(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
  });
}

// True when `path` is one or more identifiers joined by '.'.
bool IsSlotPath(std::string_view path) {
  size_t start = 0;
  while (true) {
    const size_t dot = path.find('.', start);
    if (!IsIdentifier(path.substr(start, dot == std::string_view::npos
                                             ? std::string_view::npos
                                             : dot - start))) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    start = dot + 1;
  }
}

// Splits a comma separated list into trimmed items. An empty `text` yields
// no items; an empty item between commas is kept (as "").
std::vector<std::string_view> SplitList(std::string_view text) {
  std::vector<std::string_view> items;
  if (core::Trim(text).empty()) {
    return items;
  }
  size_t start = 0;
  while (true) {
    const size_t comma = text.find(',', start);
    items.push_back(core::Trim(text.substr(
        start, comma == std::string_view::npos ? std::string_view::npos
                                               : comma - start)));
    if (comma == std::string_view::npos) {
      return items;
    }
    start = comma + 1;
  }
}

// Which categories may use a key.
enum class KeyScope {
  kAny,
  kRoot,      // lower_receiver, complete
  kComplete,  // complete
  kNonRoot,   // everything but the roots
  kBarrel,    // barrel, complete
  kMagazine,  // magazine, complete
};

bool ScopeAllows(KeyScope scope, PartCategory category) {
  switch (scope) {
    case KeyScope::kAny:
      return true;
    case KeyScope::kRoot:
      return IsRootCategory(category);
    case KeyScope::kComplete:
      return category == PartCategory::kComplete;
    case KeyScope::kNonRoot:
      return !IsRootCategory(category);
    case KeyScope::kBarrel:
      return category == PartCategory::kBarrel ||
             category == PartCategory::kComplete;
    case KeyScope::kMagazine:
      return category == PartCategory::kMagazine ||
             category == PartCategory::kComplete;
  }
  return false;
}

std::string_view ScopeText(KeyScope scope) {
  switch (scope) {
    case KeyScope::kAny:
      return "any part";
    case KeyScope::kRoot:
      return "lower_receiver and complete parts";
    case KeyScope::kComplete:
      return "complete parts";
    case KeyScope::kNonRoot:
      return "parts other than lower_receiver and complete";
    case KeyScope::kBarrel:
      return "barrel and complete parts";
    case KeyScope::kMagazine:
      return "magazine and complete parts";
  }
  return "?";
}

// Lower bound a number must respect.
enum class Bound { kNone, kNonNegative, kPositive };

// A part section being read.
struct PartSection {
  RawPartDef raw;
  bool has_category = false;
  bool has_fire_modes = false;
  bool has_fire_rate = false;
  struct ScopedKey {
    std::string key;
    KeyScope scope;
    int line;
  };
  // Keys whose validity depends on the category, checked when the section
  // ends (the category may come after them).
  std::vector<ScopedKey> scoped;
};

// Reads one file. Holds the section being read; stops at the first error.
class FileParser {
 public:
  explicit FileParser(std::string_view file_name) {
    result_.file = std::string(file_name);
  }

  FileResult Parse(std::string_view text) {
    if (text.starts_with(kUtf8Bom)) {
      text.remove_prefix(kUtf8Bom.size());
    }
    int line_number = 0;
    size_t line_start = 0;
    while (line_start <= text.size()) {
      size_t line_end = text.find('\n', line_start);
      if (line_end == std::string_view::npos) {
        line_end = text.size();
      }
      std::string_view line = text.substr(line_start, line_end - line_start);
      line_start = line_end + 1;
      ++line_number;
      line = core::Trim(line.substr(0, line.find('#')));
      if (line.empty()) {
        continue;
      }
      if (MaybeError error = ParseLine(line, line_number)) {
        return FileResult::Error(std::move(*error));
      }
    }
    if (MaybeError error = EndSection()) {
      return FileResult::Error(std::move(*error));
    }
    return FileResult::Ok(std::move(result_));
  }

 private:
  CatalogError Error(int line, std::string message) const {
    return CatalogError{result_.file, line, std::move(message)};
  }

  MaybeError ParseLine(std::string_view line, int line_number) {
    if (line.front() == '[') {
      if (MaybeError error = EndSection()) {
        return error;
      }
      return BeginSection(line, line_number);
    }
    const size_t equals = line.find('=');
    if (equals == std::string_view::npos) {
      return Error(line_number,
                   std::format("expected 'key = value', got '{}'", line));
    }
    const std::string_view key = core::Trim(line.substr(0, equals));
    const std::string_view value = core::Trim(line.substr(equals + 1));
    if (key.empty()) {
      return Error(line_number, "missing key before '='");
    }
    if (!part_ && !preset_ && !extension_) {
      return Error(line_number,
                   std::format("'{}' outside a [part], [preset] or [extend] "
                               "section",
                               key));
    }
    if (std::ranges::find(keys_, key) != keys_.end()) {
      return Error(line_number, std::format("'{}' is set twice", key));
    }
    keys_.emplace_back(key);
    if (extension_) {
      return ExtensionKey(key, value, line_number);
    }
    return part_ ? PartKey(key, value, line_number)
                 : PresetKey(key, value, line_number);
  }

  MaybeError BeginSection(std::string_view line, int line_number) {
    if (line.back() != ']') {
      return Error(line_number, std::format("bad section header '{}'", line));
    }
    const std::string_view inside = core::Trim(line.substr(1, line.size() - 2));
    const size_t space = inside.find_first_of(" \t");
    const std::string_view kind = inside.substr(0, space);
    const std::string_view id = space == std::string_view::npos
                                    ? std::string_view()
                                    : core::Trim(inside.substr(space));
    if (!IsIdentifier(id)) {
      return Error(line_number,
                   std::format("bad id '{}' (expected [a-z0-9_]+)", id));
    }
    keys_.clear();
    if (kind == "part") {
      part_.emplace();
      part_->raw.def.id = std::string(id);
      part_->raw.line = line_number;
      return std::nullopt;
    }
    if (kind == "preset") {
      preset_.emplace();
      preset_->id = std::string(id);
      preset_->line = line_number;
      return std::nullopt;
    }
    if (kind == "extend") {
      extension_.emplace();
      extension_->part = std::string(id);
      extension_->line = line_number;
      return std::nullopt;
    }
    return Error(line_number,
                 std::format("unknown section '{}' (expected part, preset or "
                             "extend)",
                             kind));
  }

  MaybeError EndSection() {
    if (extension_) {
      result_.extensions.push_back(std::move(*extension_));
      extension_.reset();
      return std::nullopt;
    }
    if (preset_) {
      RawPresetDef preset = std::move(*preset_);
      preset_.reset();
      if (preset.root.empty()) {
        return Error(preset.line,
                     std::format("preset '{}' has no root", preset.id));
      }
      if (preset.name.empty()) {
        preset.name = preset.id;
      }
      result_.presets.push_back(std::move(preset));
      return std::nullopt;
    }
    if (!part_) {
      return std::nullopt;
    }
    PartSection section = std::move(*part_);
    part_.reset();
    PartDef& def = section.raw.def;
    if (!section.has_category) {
      return Error(section.raw.line,
                   std::format("part '{}' has no category", def.id));
    }
    for (const PartSection::ScopedKey& scoped : section.scoped) {
      if (!ScopeAllows(scoped.scope, def.category)) {
        return Error(
            scoped.line,
            std::format("'{}' applies to {} only, not to a {} part", scoped.key,
                        ScopeText(scoped.scope), ToString(def.category)));
      }
    }
    if (IsRootCategory(def.category) &&
        (!section.has_fire_modes || !section.has_fire_rate)) {
      return Error(
          section.raw.line,
          std::format("root part '{}' needs fire_modes and fire_rate", def.id));
    }
    if (def.name.empty()) {
      def.name = def.id;
    }
    result_.parts.push_back(std::move(section.raw));
    return std::nullopt;
  }

  MaybeError ReadFloat(std::string_view key, std::string_view value,
                       int line_number, Bound bound, float& out) const {
    const std::optional<float> parsed = core::ParseFiniteFloat(value);
    if (!parsed) {
      return Error(line_number,
                   std::format("'{}' needs a number, got '{}'", key, value));
    }
    if ((bound == Bound::kNonNegative && *parsed < 0.0f) ||
        (bound == Bound::kPositive && *parsed <= 0.0f)) {
      return Error(
          line_number,
          std::format("'{}' must be {}, got '{}'", key,
                      bound == Bound::kPositive ? "positive" : "zero or more",
                      value));
    }
    out = *parsed;
    return std::nullopt;
  }

  // Remembers that `key` is limited to `scope` and reads its number.
  MaybeError ScopedFloat(std::string_view key, std::string_view value,
                         int line_number, KeyScope scope, Bound bound,
                         float& out) {
    part_->scoped.push_back({std::string(key), scope, line_number});
    return ReadFloat(key, value, line_number, bound, out);
  }

  MaybeError PartKey(std::string_view key, std::string_view value,
                     int line_number) {
    PartDef& def = part_->raw.def;
    if (key.starts_with(kSlotPrefix)) {
      return SlotKey(key.substr(kSlotPrefix.size()), value, line_number);
    }
    if (key == "name") {
      def.name = std::string(value);
      return std::nullopt;
    }
    if (key == "category") {
      const std::optional<PartCategory> category = ParsePartCategory(value);
      if (!category) {
        return Error(line_number, std::format("unknown category '{}'", value));
      }
      def.category = *category;
      part_->has_category = true;
      return std::nullopt;
    }
    if (key == "prefab") {
      def.prefab = std::string(value);
      return std::nullopt;
    }
    if (key == "weight") {
      return ReadFloat(key, value, line_number, Bound::kNonNegative,
                       def.weight);
    }
    if (key == "ergonomics") {
      return ReadFloat(key, value, line_number, Bound::kNone, def.ergonomics);
    }
    if (key == "velocity") {
      return ReadFloat(key, value, line_number, Bound::kPositive, def.velocity);
    }
    if (key == "sighting_range") {
      return ReadFloat(key, value, line_number, Bound::kNonNegative,
                       def.sighting_range);
    }
    if (key == "weapon_kind") {
      part_->scoped.push_back({std::string(key), KeyScope::kRoot, line_number});
      if (value == "pistol") {
        def.weapon_kind = WeaponKind::kPistol;
      } else if (value == "rifle") {
        def.weapon_kind = WeaponKind::kRifle;
      } else {
        return Error(
            line_number,
            std::format("unknown weapon_kind '{}' (pistol or rifle)", value));
      }
      return std::nullopt;
    }
    if (key == "recoil_vertical") {
      return ScopedFloat(key, value, line_number, KeyScope::kRoot,
                         Bound::kNonNegative, def.recoil_vertical);
    }
    if (key == "recoil_horizontal") {
      return ScopedFloat(key, value, line_number, KeyScope::kRoot,
                         Bound::kNonNegative, def.recoil_horizontal);
    }
    if (key == "fire_modes") {
      return FireModesKey(key, value, line_number);
    }
    if (key == "fire_rate") {
      part_->has_fire_rate = true;
      return ScopedFloat(key, value, line_number, KeyScope::kRoot,
                         Bound::kPositive, def.fire_rate);
    }
    if (key == "legacy_type") {
      part_->scoped.push_back(
          {std::string(key), KeyScope::kComplete, line_number});
      def.legacy_type = std::string(value);
      return std::nullopt;
    }
    if (key == "recoil") {
      return ScopedFloat(key, value, line_number, KeyScope::kNonRoot,
                         Bound::kNone, def.recoil);
    }
    if (key == "accuracy") {
      return ScopedFloat(key, value, line_number, KeyScope::kNonRoot,
                         Bound::kNone, def.accuracy);
    }
    if (key == "accuracy_moa") {
      return ScopedFloat(key, value, line_number, KeyScope::kBarrel,
                         Bound::kNonNegative, def.accuracy_moa);
    }
    if (key == "muzzle_velocity") {
      return ScopedFloat(key, value, line_number, KeyScope::kBarrel,
                         Bound::kNonNegative, def.muzzle_velocity);
    }
    if (key == "capacity") {
      part_->scoped.push_back(
          {std::string(key), KeyScope::kMagazine, line_number});
      const std::optional<int> capacity = core::ParseInt(value);
      if (!capacity || *capacity < 0) {
        return Error(line_number,
                     std::format("'capacity' needs a whole number of 0 or "
                                 "more, got '{}'",
                                 value));
      }
      def.capacity = *capacity;
      return std::nullopt;
    }
    return Error(line_number, std::format("unknown key '{}'", key));
  }

  MaybeError FireModesKey(std::string_view key, std::string_view value,
                          int line_number) {
    PartDef& def = part_->raw.def;
    part_->scoped.push_back({std::string(key), KeyScope::kRoot, line_number});
    part_->has_fire_modes = true;
    const std::vector<std::string_view> names = SplitList(value);
    if (names.empty()) {
      return Error(line_number, "'fire_modes' lists no mode");
    }
    for (const std::string_view name : names) {
      const std::optional<FireMode> mode = ParseFireMode(name);
      if (!mode) {
        return Error(line_number,
                     std::format("unknown fire mode '{}' (semi, auto or "
                                 "double_action)",
                                 name));
      }
      if (std::ranges::find(def.fire_modes, *mode) != def.fire_modes.end()) {
        return Error(line_number,
                     std::format("fire mode '{}' is listed twice", name));
      }
      def.fire_modes.push_back(*mode);
    }
    return std::nullopt;
  }

  MaybeError SlotKey(std::string_view name, std::string_view value,
                     int line_number) {
    if (!IsIdentifier(name)) {
      return Error(line_number, std::format("bad slot name '{}'", name));
    }
    const size_t space = value.find_first_of(" \t");
    const std::string_view kind = value.substr(0, space);
    RawSlotDef slot{.name = std::string(name), .line = line_number};
    if (kind == "required") {
      slot.required = true;
    } else if (kind != "optional") {
      return Error(line_number,
                   std::format("slot '{}' must start with 'required' or "
                               "'optional', got '{}'",
                               name, kind));
    }
    const std::string_view list = space == std::string_view::npos
                                      ? std::string_view()
                                      : value.substr(space);
    if (MaybeError error = ReadIds(list, name, line_number, slot.accepted)) {
      return error;
    }
    part_->raw.slots.push_back(std::move(slot));
    return std::nullopt;
  }

  // Appends the part ids of the comma separated `list` (may be empty) for
  // slot `slot` to `out`.
  MaybeError ReadIds(std::string_view list, std::string_view slot,
                     int line_number, std::vector<std::string>& out) const {
    for (const std::string_view id : SplitList(list)) {
      if (!IsIdentifier(id)) {
        return Error(line_number,
                     std::format("bad part id '{}' in slot '{}'", id, slot));
      }
      out.emplace_back(id);
    }
    return std::nullopt;
  }

  // "slot.<name> += id, id": the key arrives as "slot.<name> +".
  MaybeError ExtensionKey(std::string_view key, std::string_view value,
                          int line_number) {
    if (!key.ends_with('+')) {
      return Error(line_number,
                   std::format("an [extend] section takes 'slot.<name> += "
                               "<ids>' only, got '{}'",
                               key));
    }
    key = core::Trim(key.substr(0, key.size() - 1));
    if (!key.starts_with(kSlotPrefix) ||
        !IsIdentifier(key.substr(kSlotPrefix.size()))) {
      return Error(line_number, std::format("bad slot key '{}'", key));
    }
    RawSlotExtension slot{.slot = std::string(key.substr(kSlotPrefix.size())),
                          .line = line_number};
    if (MaybeError error =
            ReadIds(value, slot.slot, line_number, slot.accepted)) {
      return error;
    }
    if (slot.accepted.empty()) {
      return Error(line_number, std::format("'{} +=' adds no part", key));
    }
    extension_->slots.push_back(std::move(slot));
    return std::nullopt;
  }

  MaybeError PresetKey(std::string_view key, std::string_view value,
                       int line_number) {
    if (key == "name") {
      preset_->name = std::string(value);
      return std::nullopt;
    }
    if (!IsIdentifier(value)) {
      return Error(line_number,
                   std::format("'{}' needs a part id, got '{}'", key, value));
    }
    if (key == "root") {
      preset_->root = std::string(value);
      preset_->root_line = line_number;
      return std::nullopt;
    }
    if (!IsSlotPath(key)) {
      return Error(line_number,
                   std::format("unknown key '{}' (expected name, root or a "
                               "slot path)",
                               key));
    }
    preset_->entries.push_back(
        {std::string(key), std::string(value), line_number});
    return std::nullopt;
  }

  PartCatalogFile result_;
  std::optional<PartSection> part_;
  std::optional<RawPresetDef> preset_;
  std::optional<RawPartExtension> extension_;
  std::vector<std::string> keys_;  // of the current section
};

struct Place {
  std::string file;
  int line = 0;
};

std::string PlaceText(const Place& place) {
  return std::format("{}:{}", place.file, place.line);
}

// Resolves part ids against `catalog`; errors name `file`.
class Resolver {
 public:
  explicit Resolver(const PartCatalog& catalog) : catalog_(catalog) {}

  core::Result<PartId, CatalogError> Part(std::string_view id,
                                          const std::string& file,
                                          int line) const {
    if (const std::optional<PartId> part = catalog_.Find(id)) {
      return core::Result<PartId, CatalogError>::Ok(*part);
    }
    return core::Result<PartId, CatalogError>::Error(
        {file, line, std::format("unknown part '{}'", id)});
  }

 private:
  const PartCatalog& catalog_;
};

// Adds the ids of the [extend] sections of `files` to the slot filters of
// `parts` (already resolved; `resolver` knows every part).
MaybeError ApplyExtensions(std::span<const PartCatalogFile> files,
                           const Resolver& resolver,
                           std::vector<PartDef>& parts) {
  for (const PartCatalogFile& file : files) {
    for (const RawPartExtension& extension : file.extensions) {
      const auto target =
          resolver.Part(extension.part, file.file, extension.line);
      if (!target.ok()) {
        return target.error();
      }
      PartDef& def = parts[static_cast<size_t>(target.value().index)];
      for (const RawSlotExtension& added : extension.slots) {
        const int slot = def.FindSlot(added.slot);
        if (slot < 0) {
          return CatalogError{
              file.file, added.line,
              std::format("part '{}' has no slot '{}'", def.id, added.slot)};
        }
        std::vector<PartId>& accepted =
            def.slots[static_cast<size_t>(slot)].accepted;
        for (const std::string& id : added.accepted) {
          const auto part = resolver.Part(id, file.file, added.line);
          if (!part.ok()) {
            return part.error();
          }
          if (std::ranges::find(accepted, part.value()) == accepted.end()) {
            accepted.push_back(part.value());
          }
        }
      }
    }
  }
  return std::nullopt;
}

// Checks `preset` by building it entry by entry, parents first, so an error
// names the entry's line.
MaybeError CheckPreset(const PartCatalog& catalog, const PresetDef& preset,
                       int root_line) {
  if (!IsRootCategory(catalog.part(preset.root).category)) {
    return CatalogError{
        preset.file, root_line,
        std::format("root '{}' is not a lower_receiver or complete part",
                    catalog.part(preset.root).id)};
  }
  std::vector<const PresetEntry*> order;
  for (const PresetEntry& entry : preset.entries) {
    order.push_back(&entry);
  }
  std::ranges::stable_sort(order, {}, [](const PresetEntry* entry) {
    return std::ranges::count(entry->path, '.');
  });
  WeaponBuild build(catalog, preset.root);
  for (const PresetEntry* entry : order) {
    auto replaced =
        Install(catalog, build, entry->path, WeaponBuild(catalog, entry->part));
    if (!replaced.ok()) {
      return CatalogError{preset.file, entry->line, replaced.error().message};
    }
  }
  return std::nullopt;
}

}  // namespace

std::string CatalogError::ToString() const {
  if (line > 0) {
    return std::format("{}:{}: {}", file, line, message);
  }
  return std::format("{}: {}", file, message);
}

core::Result<PartCatalogFile, CatalogError> ParsePartCatalog(
    std::string_view text, std::string_view file_name) {
  return FileParser(file_name).Parse(text);
}

core::Result<PartCatalog, CatalogError> MergePartCatalogs(
    std::span<const PartCatalogFile> files) {
  // Parts first, without slots, so slot filters can refer to any file.
  std::vector<PartDef> parts;
  std::map<std::string, Place, std::less<>> part_places;
  for (const PartCatalogFile& file : files) {
    for (const RawPartDef& raw : file.parts) {
      const Place place{file.file, raw.line};
      const auto [it, added] = part_places.emplace(raw.def.id, place);
      if (!added) {
        return CatalogResult::Error(
            {file.file, raw.line,
             std::format("part '{}' is already defined at {}", raw.def.id,
                         PlaceText(it->second))});
      }
      parts.push_back(raw.def);
      parts.back().slots.clear();
    }
  }
  const PartCatalog unresolved(parts, {});
  const Resolver resolver(unresolved);
  size_t part_index = 0;
  for (const PartCatalogFile& file : files) {
    for (const RawPartDef& raw : file.parts) {
      PartDef& def = parts[part_index++];
      for (const RawSlotDef& raw_slot : raw.slots) {
        if (def.FindSlot(raw_slot.name) >= 0) {
          return CatalogResult::Error(
              {file.file, raw_slot.line,
               std::format("slot '{}' is defined twice", raw_slot.name)});
        }
        SlotDef slot{.name = raw_slot.name, .required = raw_slot.required};
        for (const std::string& id : raw_slot.accepted) {
          const auto part = resolver.Part(id, file.file, raw_slot.line);
          if (!part.ok()) {
            return CatalogResult::Error(part.error());
          }
          slot.accepted.push_back(part.value());
        }
        def.slots.push_back(std::move(slot));
      }
    }
  }
  if (MaybeError error = ApplyExtensions(files, resolver, parts)) {
    return CatalogResult::Error(std::move(*error));
  }

  const PartCatalog with_slots(std::move(parts), {});
  std::vector<PresetDef> presets;
  std::map<std::string, Place, std::less<>> preset_places;
  for (const PartCatalogFile& file : files) {
    for (const RawPresetDef& raw : file.presets) {
      const auto [it, added] =
          preset_places.emplace(raw.id, Place{file.file, raw.line});
      if (!added) {
        return CatalogResult::Error(
            {file.file, raw.line,
             std::format("preset '{}' is already defined at {}", raw.id,
                         PlaceText(it->second))});
      }
      PresetDef preset{
          .id = raw.id, .name = raw.name, .file = file.file, .line = raw.line};
      const auto root = resolver.Part(raw.root, file.file, raw.root_line);
      if (!root.ok()) {
        return CatalogResult::Error(root.error());
      }
      preset.root = root.value();
      for (const RawPresetEntry& raw_entry : raw.entries) {
        const auto part =
            resolver.Part(raw_entry.part, file.file, raw_entry.line);
        if (!part.ok()) {
          return CatalogResult::Error(part.error());
        }
        preset.entries.push_back(
            {raw_entry.path, part.value(), raw_entry.line});
      }
      if (MaybeError error = CheckPreset(with_slots, preset, raw.root_line)) {
        return CatalogResult::Error(std::move(*error));
      }
      presets.push_back(std::move(preset));
    }
  }
  std::vector<PartDef> final_parts(with_slots.parts().begin(),
                                   with_slots.parts().end());
  return CatalogResult::Ok(
      PartCatalog(std::move(final_parts), std::move(presets)));
}

core::Result<PartCatalog, CatalogError> LoadPartCatalog(
    const std::string& directory) {
  const std::optional<std::vector<std::string>> names =
      platform::ListDirectory(directory);
  if (!names) {
    return CatalogResult::Error(
        {directory, 0, "cannot read the catalogue directory"});
  }
  std::vector<PartCatalogFile> files;
  for (const std::string& name : *names) {
    const std::string path = core::JoinPath(directory, name);
    if (!name.ends_with(kCatalogExtension) ||
        name.ends_with(kGeneratedExtension) || !platform::IsRegularFile(path)) {
      continue;
    }
    const auto bytes = platform::ReadFile(path, kMaxCatalogFileSize);
    if (!bytes.ok()) {
      return CatalogResult::Error({path, 0, platform::ToString(bytes.error())});
    }
    std::string text(bytes.value().size(), '\0');
    std::ranges::transform(bytes.value(), text.begin(),
                           [](std::byte b) { return static_cast<char>(b); });
    auto parsed = ParsePartCatalog(text, path);
    if (!parsed.ok()) {
      return CatalogResult::Error(parsed.error());
    }
    files.push_back(std::move(parsed).value());
  }
  return MergePartCatalogs(files);
}

}  // namespace blue::weapon::parts
