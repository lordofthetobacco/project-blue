#ifndef BLUE_WEAPON_PARTS_PART_CATALOG_H_
#define BLUE_WEAPON_PARTS_PART_CATALOG_H_

#include <compare>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "weapon/firearm.h"

// Weapon part ("gunsmith") catalogue: every part a weapon can be built from,
// the slots each part offers, and ready-made builds (presets). Pure data; the
// text format is read by part_catalog_file.h.

namespace blue::weapon::parts {

// Index of a part in its PartCatalog. Default-constructed: no part.
struct PartId {
  int index = -1;

  [[nodiscard]] bool valid() const { return index >= 0; }
  friend auto operator<=>(const PartId&, const PartId&) = default;
};

enum class PartCategory {
  kLowerReceiver,
  kSlide,
  kBarrel,
  kMagazine,
  kSightRear,
  kSightFront,
  kOptic,
  kMuzzle,
  kTactical,
  kComplete,  // a single-piece weapon (no slots needed)
};

// Returns the category spelled `name` ("lower_receiver", ...), or nullopt.
std::optional<PartCategory> ParsePartCategory(std::string_view name);

// Returns the catalogue spelling of `category`.
std::string_view ToString(PartCategory category);

// True for the categories a weapon can be built on: lower_receiver and
// complete.
bool IsRootCategory(PartCategory category);

// Returns the fire mode spelled `name` ("semi", "auto", "double_action"),
// or nullopt.
std::optional<FireMode> ParseFireMode(std::string_view name);

// Returns the catalogue spelling of `mode`.
std::string_view ToString(FireMode mode);

// A slot a part offers to a child part.
struct SlotDef {
  std::string name;
  bool required = false;  // the weapon is not functional while it is empty
  std::vector<PartId> accepted;  // the filter, in file order

  // True when `part` passes the filter.
  [[nodiscard]] bool Accepts(PartId part) const;
};

// One part as the catalogue describes it. Which fields apply depends on the
// category; unused fields keep their defaults.
struct PartDef {
  std::string id;    // [a-z0-9_]+
  std::string name;  // shown to the player; the id when the file has none
  PartCategory category = PartCategory::kTactical;
  std::string prefab;   // relative to the asset root; empty: no model
  float weight = 0.0f;  // kg
  // Root: base value (0..100). Other parts: change added to the root's.
  float ergonomics = 0.0f;

  // Root parts only.
  WeaponKind weapon_kind = WeaponKind::kPistol;
  float recoil_vertical = 0.0f;
  float recoil_horizontal = 0.0f;
  std::vector<FireMode> fire_modes;  // the first is the default
  float fire_rate = 0.0f;            // rounds per minute
  std::string legacy_type;           // complete parts: existing weapon type

  // Non-root parts: percentages (recoil applies to both axes; accuracy
  // changes the MOA figure, negative = tighter).
  float recoil = 0.0f;
  float accuracy = 0.0f;

  // Barrels (and complete parts).
  float accuracy_moa = 0.0f;     // base dispersion
  float muzzle_velocity = 0.0f;  // m/s
  float velocity = 1.0f;         // any part: factor on muzzle velocity
  int capacity = 0;              // magazines (and complete parts)
  float sighting_range = 0.0f;   // metres, 0 = none

  std::vector<SlotDef> slots;  // in file order

  // Index of the slot called `slot_name`, or -1.
  [[nodiscard]] int FindSlot(std::string_view slot_name) const;
};

// One "slot path = part" line of a preset.
struct PresetEntry {
  std::string path;  // slot names joined by '.', e.g. "slide.sight_rear"
  PartId part;
  int line = 0;  // where the entry is written, for messages
};

// A ready-built weapon.
struct PresetDef {
  std::string id;
  std::string name;
  PartId root;
  std::vector<PresetEntry> entries;  // in file order
  std::string file;                  // where the preset is written
  int line = 0;
};

// The catalogue: parts and presets with lookup by id. Copyable value type.
// Not thread-safe for mutation; const use is safe from several threads.
class PartCatalog {
 public:
  PartCatalog() = default;
  // Takes the parts and presets as they are; ids must be unique within
  // each list and every PartId must index `parts` (the loader checks this).
  PartCatalog(std::vector<PartDef> parts, std::vector<PresetDef> presets);
  PartCatalog(const PartCatalog&) = default;
  PartCatalog& operator=(const PartCatalog&) = default;
  PartCatalog(PartCatalog&&) = default;
  PartCatalog& operator=(PartCatalog&&) = default;

  [[nodiscard]] std::optional<PartId> Find(std::string_view id) const;
  // `id` must be valid for this catalogue.
  [[nodiscard]] const PartDef& part(PartId id) const;
  [[nodiscard]] std::span<const PartDef> parts() const { return parts_; }
  [[nodiscard]] int part_count() const {
    return static_cast<int>(parts_.size());
  }
  // True when `id` indexes a part of this catalogue.
  [[nodiscard]] bool Contains(PartId id) const {
    return id.index >= 0 && id.index < part_count();
  }

  // Returns the preset called `id`, or nullptr. Points into this catalogue.
  [[nodiscard]] const PresetDef* FindPreset(std::string_view id) const;
  [[nodiscard]] std::span<const PresetDef> presets() const { return presets_; }

 private:
  std::vector<PartDef> parts_;
  std::vector<PresetDef> presets_;
  std::map<std::string, int, std::less<>> part_index_;
  std::map<std::string, int, std::less<>> preset_index_;
};

}  // namespace blue::weapon::parts

#endif  // BLUE_WEAPON_PARTS_PART_CATALOG_H_
