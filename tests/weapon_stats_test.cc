#include "weapon/parts/weapon_stats.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tests/part_catalog_samples.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/parts/weapon_build.h"

namespace blue::weapon::parts {
namespace {

using doctest::Approx;
using testing::GlockCatalog;
using testing::Id;

WeaponBuild PresetBuild(const PartCatalog& catalog, std::string_view id) {
  const PresetDef* preset = catalog.FindPreset(id);
  REQUIRE(preset != nullptr);
  auto build = MakeBuild(catalog, *preset);
  REQUIRE_MESSAGE(build.ok(), build.error().message);
  return std::move(build).value();
}

void InstallPart(const PartCatalog& catalog, WeaponBuild& build,
                 std::string_view path, std::string_view part) {
  auto installed =
      Install(catalog, build, path, WeaponBuild(catalog, Id(catalog, part)));
  REQUIRE_MESSAGE(installed.ok(), installed.error().message);
}

// Extremes for the clamps: a frame near the ergonomics limit and parts that
// push past every bound.
constexpr std::string_view kClampCatalogText = R"(
[part frame]
category = lower_receiver
fire_modes = double_action, semi
fire_rate = 500
ergonomics = 98
recoil_vertical = 100
recoil_horizontal = 50
slot.grip = optional grip_good, grip_bad
slot.muzzle = optional brake
slot.barrel = optional barrel_x

[part grip_good]
category = tactical
ergonomics = 5

[part grip_bad]
category = tactical
ergonomics = -150

[part brake]
category = muzzle
recoil = -95
accuracy = -150

[part barrel_x]
category = barrel
accuracy_moa = 2
muzzle_velocity = 300
)";

PartCatalog ClampCatalog() {
  auto file = ParsePartCatalog(kClampCatalogText, "clamp.cfg");
  REQUIRE_MESSAGE(file.ok(), file.error().ToString());
  const std::array<PartCatalogFile, 1> files = {std::move(file).value()};
  auto catalog = MergePartCatalogs(files);
  REQUIRE_MESSAGE(catalog.ok(), catalog.error().ToString());
  return std::move(catalog).value();
}

TEST_CASE("an empty build has default stats") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponStats stats = ComputeStats(catalog, WeaponBuild());
  CHECK(stats == WeaponStats{});
  CHECK_FALSE(stats.functional);
}

TEST_CASE("the stock Glock adds up as computed by hand") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponStats stats =
      ComputeStats(catalog, PresetBuild(catalog, "glock17"));
  // frame 0.21 + slide 0.25 + sights 0.01 + 0.005 + barrel 0.18 + mag 0.08
  CHECK(stats.weight == Approx(0.735f));
  CHECK(stats.ergonomics == Approx(91.0f));  // 90 + 3 - 2
  // 311 / 260 x (1 - 2.9 / 100)
  CHECK(stats.recoil_vertical == Approx(301.981f));
  CHECK(stats.recoil_horizontal == Approx(252.46f));
  CHECK(stats.accuracy_moa == Approx(4.4f));
  CHECK(stats.sighting_range == Approx(50.0f));
  CHECK(stats.muzzle_velocity == Approx(375.0f));
  CHECK(stats.magazine_capacity == 17);
  CHECK(stats.weapon_kind == WeaponKind::kPistol);
  CHECK(stats.fire_modes == FireModeBit(FireMode::kSemi));
  CHECK(stats.default_fire_mode == FireMode::kSemi);
  CHECK(stats.fire_rate == Approx(1100.0f));
  CHECK(stats.functional);
  CHECK(stats.missing_required.empty());
}

TEST_CASE("a fully modded Glock adds up as computed by hand") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = PresetBuild(catalog, "glock17");
  WeaponBuild slide(catalog, Id(catalog, "glock17_slide_mos"));
  InstallPart(catalog, slide, "sight_rear", "red_dot");
  InstallPart(catalog, slide, "sight_front", "glock_sight_front");
  REQUIRE(Install(catalog, build, "slide", slide).ok());
  WeaponBuild barrel(catalog, Id(catalog, "glock17_barrel_threaded"));
  InstallPart(catalog, barrel, "muzzle", "suppressor_9mm");
  REQUIRE(Install(catalog, build, "barrel", barrel).ok());
  InstallPart(catalog, build, "magazine", "glock_mag_33");
  InstallPart(catalog, build, "tactical", "olight_pl2");

  const WeaponStats stats = ComputeStats(catalog, build);
  // 0.21 + 0.24 + 0.05 + 0.005 + 0.19 + 0.2 + 0.17 + 0.1
  CHECK(stats.weight == Approx(1.165f));
  CHECK(stats.ergonomics == Approx(65.0f));  // 90 + 1 - 2 - 3 - 12 - 8 - 1
  // recoil % = -3.5 - 15 + 2 = -16.5
  CHECK(stats.recoil_vertical == Approx(311.0f * 0.835f));
  CHECK(stats.recoil_horizontal == Approx(260.0f * 0.835f));
  CHECK(stats.accuracy_moa == Approx(4.1f * 0.95f));
  CHECK(stats.sighting_range == Approx(100.0f));  // red dot beats nothing
  CHECK(stats.muzzle_velocity == Approx(375.0f * 1.02f * 0.95f));
  CHECK(stats.magazine_capacity == 33);
  CHECK(stats.functional);
}

TEST_CASE("missing parts: not functional, no barrel figures, no magazine") {
  const PartCatalog catalog = GlockCatalog();
  WeaponBuild build = PresetBuild(catalog, "glock17");
  REQUIRE(Remove(catalog, build, "magazine").ok());
  WeaponStats stats = ComputeStats(catalog, build);
  CHECK(stats.functional);  // the magazine slot is optional
  CHECK(stats.magazine_capacity == 0);

  REQUIRE(Remove(catalog, build, "barrel").ok());
  REQUIRE(Remove(catalog, build, "slide.sight_rear").ok());
  stats = ComputeStats(catalog, build);
  CHECK_FALSE(stats.functional);
  CHECK(stats.missing_required ==
        std::vector<std::string>{"slide.sight_rear", "barrel"});
  CHECK(stats.accuracy_moa == 0.0f);
  CHECK(stats.muzzle_velocity == 0.0f);
  CHECK(stats.sighting_range == 0.0f);
  CHECK(stats.recoil_vertical == Approx(311.0f));  // no recoil modifiers
}

TEST_CASE("a complete part stands in for its own barrel and magazine") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponStats stats = ComputeStats(catalog, PresetBuild(catalog, "m16"));
  CHECK(stats.weight == Approx(3.99f));
  CHECK(stats.ergonomics == Approx(50.0f));
  CHECK(stats.recoil_vertical == Approx(150.0f));
  CHECK(stats.recoil_horizontal == Approx(200.0f));
  CHECK(stats.accuracy_moa == Approx(2.5f));
  CHECK(stats.sighting_range == Approx(300.0f));
  CHECK(stats.muzzle_velocity == Approx(948.0f));
  CHECK(stats.magazine_capacity == 30);
  CHECK(stats.weapon_kind == WeaponKind::kRifle);
  CHECK(stats.fire_modes ==
        (FireModeBit(FireMode::kSemi) | FireModeBit(FireMode::kAuto)));
  CHECK(stats.default_fire_mode == FireMode::kSemi);
  CHECK(stats.fire_rate == Approx(800.0f));
  CHECK(stats.functional);
}

TEST_CASE("ergonomics clamp to 0..100") {
  const PartCatalog catalog = ClampCatalog();
  WeaponBuild build(catalog, Id(catalog, "frame"));
  CHECK(ComputeStats(catalog, build).ergonomics == Approx(98.0f));
  InstallPart(catalog, build, "grip", "grip_good");
  CHECK(ComputeStats(catalog, build).ergonomics == Approx(100.0f));  // 103
  InstallPart(catalog, build, "grip", "grip_bad");
  CHECK(ComputeStats(catalog, build).ergonomics == Approx(0.0f));  // -52
}

TEST_CASE("recoil floors at 10 % of the root, accuracy at 0") {
  const PartCatalog catalog = ClampCatalog();
  WeaponBuild build(catalog, Id(catalog, "frame"));
  InstallPart(catalog, build, "barrel", "barrel_x");
  InstallPart(catalog, build, "muzzle", "brake");
  const WeaponStats stats = ComputeStats(catalog, build);
  CHECK(stats.recoil_vertical == Approx(10.0f));   // 100 x 0.05 -> 10
  CHECK(stats.recoil_horizontal == Approx(5.0f));  // 50 x 0.05 -> 5
  CHECK(stats.accuracy_moa == Approx(0.0f));       // 2 x (1 - 1.5) -> 0
  CHECK(stats.muzzle_velocity == Approx(300.0f));
  CHECK(stats.fire_modes ==
        (FireModeBit(FireMode::kSemi) | FireModeBit(FireMode::kDoubleAction)));
  CHECK(stats.default_fire_mode == FireMode::kDoubleAction);
  CHECK(stats.functional);  // no required slots
}

TEST_CASE("StatsDelta previews an install on a copy") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponBuild build = PresetBuild(catalog, "glock17");
  const auto delta =
      StatsDelta(catalog, build, "magazine",
                 WeaponBuild(catalog, Id(catalog, "glock_mag_33")));
  REQUIRE(delta.ok());
  CHECK(delta.value().before == ComputeStats(catalog, build));
  CHECK(delta.value().before.magazine_capacity == 17);
  CHECK(delta.value().after.magazine_capacity == 33);
  CHECK(delta.value().after.ergonomics == Approx(83.0f));  // 91 - 8
  // -2.9 + 2 = -0.9 %
  CHECK(delta.value().after.recoil_vertical == Approx(311.0f * 0.991f));
  CHECK(delta.value().after.weight == Approx(0.735f - 0.08f + 0.17f));

  CHECK(StatsDelta(catalog, build, "magazine",
                   WeaponBuild(catalog, Id(catalog, "red_dot")))
            .error()
            .message == "slot 'magazine' does not accept part 'red_dot'");
  CHECK(build == PresetBuild(catalog, "glock17"));
}

}  // namespace
}  // namespace blue::weapon::parts
