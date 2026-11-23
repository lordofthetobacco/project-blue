#ifndef BLUE_TESTS_PART_CATALOG_SAMPLES_H_
#define BLUE_TESTS_PART_CATALOG_SAMPLES_H_

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <ostream>
#include <string_view>
#include <utility>

#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"

namespace blue::weapon::parts::testing {

// A small pistol catalogue in the contract's format, with a single-piece
// rifle. Numbers are test values, not the shipped catalogue.
inline constexpr std::string_view kGlockCatalogText = R"(# Test catalogue
[part glock17_frame]
name = Glock 17 frame
category = lower_receiver
prefab = weapons/glock17/parts/frame.pbo     # relative to the asset root
weight = 0.21
ergonomics = 90
weapon_kind = pistol
recoil_vertical = 311
recoil_horizontal = 260
fire_modes = semi
fire_rate = 1100
slot.slide = required glock17_slide, glock17_slide_mos
slot.barrel = required glock17_barrel, glock17_barrel_threaded
slot.magazine = optional glock_mag_17, glock_mag_21, glock_mag_33
slot.tactical = optional olight_pl2

[part glock17_slide]
name = Glock 17 slide
category = slide
weight = 0.25
ergonomics = 3
slot.sight_rear = required glock_sight_rear
slot.sight_front = required glock_sight_front

[part glock17_slide_mos]
category = slide
weight = 0.24
ergonomics = 1
slot.sight_rear = optional glock_sight_rear, red_dot
slot.sight_front = optional glock_sight_front

[part glock_sight_rear]
category = sight_rear
weight = 0.01
sighting_range = 50

[part glock_sight_front]
category = sight_front
weight = 0.005

[part red_dot]
category = optic
weight = 0.05
ergonomics = -2
sighting_range = 100

[part glock17_barrel]
category = barrel
weight = 0.18
ergonomics = -2
recoil = -2.9
accuracy = 0
accuracy_moa = 4.4
muzzle_velocity = 375

[part glock17_barrel_threaded]
category = barrel
weight = 0.19
ergonomics = -3
recoil = -3.5
accuracy_moa = 4.1
muzzle_velocity = 375
velocity = 1.02
slot.muzzle = optional suppressor_9mm

[part suppressor_9mm]
category = muzzle
weight = 0.2
ergonomics = -12
recoil = -15
accuracy = -5
velocity = 0.95

[part glock_mag_17]
category = magazine
weight = 0.08
capacity = 17

[part glock_mag_21]
category = magazine
weight = 0.1
ergonomics = -4
capacity = 21

[part glock_mag_33]
category = magazine
weight = 0.17
ergonomics = -8
recoil = 2
capacity = 33

[part olight_pl2]
category = tactical
weight = 0.1
ergonomics = -1

[part m16]
name = M16A2
category = complete
weapon_kind = rifle
weight = 3.99
ergonomics = 50
recoil_vertical = 150
recoil_horizontal = 200
fire_modes = semi, auto
fire_rate = 800
legacy_type = m16
accuracy_moa = 2.5
muzzle_velocity = 948
capacity = 30
sighting_range = 300

[preset glock17]
name = Glock 17
root = glock17_frame
slide.sight_rear = glock_sight_rear
slide = glock17_slide
slide.sight_front = glock_sight_front
barrel = glock17_barrel
magazine = glock_mag_17

[preset m16]
root = m16
)";

// Parses and merges kGlockCatalogText; fails the test on error.
inline PartCatalog GlockCatalog() {
  auto file = ParsePartCatalog(kGlockCatalogText, "glock.cfg");
  REQUIRE_MESSAGE(file.ok(), file.error().ToString());
  const std::array<PartCatalogFile, 1> files = {std::move(file).value()};
  auto catalog = MergePartCatalogs(files);
  REQUIRE_MESSAGE(catalog.ok(), catalog.error().ToString());
  return std::move(catalog).value();
}

// Returns the id of the part called `id`; fails the test if there is none.
inline PartId Id(const PartCatalog& catalog, std::string_view id) {
  const std::optional<PartId> part = catalog.Find(id);
  REQUIRE_MESSAGE(part.has_value(), id);
  return *part;
}

}  // namespace blue::weapon::parts::testing

#endif  // BLUE_TESTS_PART_CATALOG_SAMPLES_H_
