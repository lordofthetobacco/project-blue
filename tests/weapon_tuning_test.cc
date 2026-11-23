#include "weapon/parts/weapon_tuning.h"

#include <doctest/doctest.h>

#include <optional>
#include <string>

#include "core/path.h"
#include "core/result.h"
#include "tests/part_catalog_samples.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_stats.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon::parts {
namespace {

using testing::GlockCatalog;
using testing::Id;

WeaponStats PresetStats(const PartCatalog& catalog, std::string_view preset) {
  const PresetDef* def = catalog.FindPreset(preset);
  REQUIRE(def != nullptr);
  core::Result<WeaponBuild, BuildError> build = MakeBuild(catalog, *def);
  REQUIRE_MESSAGE(build.ok(), build.error().message);
  return ComputeStats(catalog, build.value());
}

void CheckSamePose(const WeaponPose& a, const WeaponPose& b) {
  CHECK(a.sight_offset == b.sight_offset);
  CHECK(a.rotation_radians == b.rotation_radians);
}

// Every field of the feel, exactly.
void CheckSameFeel(const ViewmodelTuning& a, const ViewmodelTuning& b) {
  CheckSamePose(a.hip, b.hip);
  CheckSamePose(a.sprint, b.sprint);
  CheckSamePose(a.reload, b.reload);
  CheckSamePose(a.holster, b.holster);
  CHECK(a.eye_relief == b.eye_relief);
  CHECK(a.aim_time == b.aim_time);
  CHECK(a.pose_rate == b.pose_rate);
  CHECK(a.zero_distance == b.zero_distance);
  CHECK(a.muzzle_velocity == b.muzzle_velocity);
  CHECK(a.gravity == b.gravity);
  CHECK(a.bullet_drag == b.bullet_drag);
  CHECK(a.sway_gain == b.sway_gain);
  CHECK(a.max_sway == b.max_sway);
  CHECK(a.sway_frequency == b.sway_frequency);
  CHECK(a.bob_frequency == b.bob_frequency);
  CHECK(a.bob_amplitude == b.bob_amplitude);
  CHECK(a.bob_reference_speed == b.bob_reference_speed);
  CHECK(a.aim_motion_scale == b.aim_motion_scale);
  CHECK(a.recoil_back_velocity == b.recoil_back_velocity);
  CHECK(a.recoil_pitch_velocity == b.recoil_pitch_velocity);
  CHECK(a.recoil_frequency == b.recoil_frequency);
  CHECK(a.look_kick_pitch == b.look_kick_pitch);
  CHECK(a.look_kick_yaw == b.look_kick_yaw);
  CHECK(a.aim_recoil_scale == b.aim_recoil_scale);
  CHECK(a.seed == b.seed);
}

// Every field of the spec but the name and the mass, exactly.
void CheckSameSpec(const FirearmSpec& a, const FirearmSpec& b) {
  CHECK(a.magazine_capacity == b.magazine_capacity);
  CHECK(a.rounds_per_minute == b.rounds_per_minute);
  CHECK(a.tactical_reload_seconds == b.tactical_reload_seconds);
  CHECK(a.empty_reload_seconds == b.empty_reload_seconds);
  CHECK(a.muzzle_velocity == b.muzzle_velocity);
  CHECK(a.bullet_mass == b.bullet_mass);
  CHECK(a.bullet_drag == b.bullet_drag);
  CHECK(a.spare_magazines == b.spare_magazines);
  CHECK(a.fire_mode == b.fire_mode);
  CHECK(a.fire_modes == b.fire_modes);
  CHECK(a.kind == b.kind);
  CHECK(a.functional == b.functional);
}

TEST_CASE("the stock Glock preset feels exactly like today's Glock 17") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponStats stats = PresetStats(catalog, "glock17");
  const BuildTuning tuning = TuneFromStats(stats, "Glock 17");
  CHECK(std::string(tuning.spec.display_name) == "Glock 17");
  CheckSameSpec(tuning.spec, Glock17Spec());
  CHECK(tuning.spec.mass == doctest::Approx(stats.weight));
  CheckSameFeel(tuning.viewmodel, Glock17ViewmodelTuning());
  CHECK(tuning.aim_fov_scale == std::optional<float>(kPistolAimFovScale));
  CHECK(tuning.switch_time_scale == 1.0f);
  CHECK(tuning.accuracy_moa == doctest::Approx(4.4f));
  REQUIRE(tuning.zero_range.has_value());
  CHECK(*tuning.zero_range ==
        ZeroRange{.min = 25.0f, .max = 50.0f, .step = 25.0f});
}

TEST_CASE("the shipped catalogue's stock Glock 17 is today's Glock 17") {
  core::Result<PartCatalog, CatalogError> catalog =
      LoadPartCatalog(core::JoinPath(BLUE_ASSETS_DIR, "weapons/catalog"));
  REQUIRE_MESSAGE(catalog.ok(), catalog.error().ToString());
  const WeaponStats stats = PresetStats(catalog.value(), "glock17");
  CHECK(stats.functional);
  const BuildTuning tuning = TuneFromStats(stats, "Glock 17");
  CheckSameSpec(tuning.spec, Glock17Spec());
  // The parts weigh what the loaded pistol does.
  CHECK(tuning.spec.mass == doctest::Approx(Glock17Spec().mass));
  CheckSameFeel(tuning.viewmodel, Glock17ViewmodelTuning());
}

TEST_CASE("recoil scales the kick, ergonomics the handling") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponStats stock = PresetStats(catalog, "glock17");
  const ViewmodelTuning base = Glock17ViewmodelTuning();

  WeaponStats stats = stock;
  stats.recoil_vertical *= 2.0f;
  stats.recoil_horizontal *= 0.5f;
  stats.ergonomics -= 10.0f;
  const BuildTuning tuning = TuneFromStats(stats, "x");
  CHECK(tuning.viewmodel.look_kick_pitch ==
        doctest::Approx(2.0f * base.look_kick_pitch));
  CHECK(tuning.viewmodel.recoil_pitch_velocity == 0.0f);  // Glock: 0 x 2
  CHECK(tuning.viewmodel.look_kick_yaw ==
        doctest::Approx(0.5f * base.look_kick_yaw));
  CHECK(tuning.switch_time_scale == doctest::Approx(1.1f));
  CHECK(tuning.viewmodel.aim_time == doctest::Approx(1.1f * base.aim_time));
  CHECK(tuning.viewmodel.sway_gain == doctest::Approx(1.1f * base.sway_gain));

  CHECK(HandlingScale(91.0f, 91.0f) == 1.0f);
  CHECK(HandlingScale(100.0f, 91.0f) == doctest::Approx(0.91f));
  CHECK(HandlingScale(0.0f, 91.0f) == doctest::Approx(1.91f));
  CHECK(HandlingScale(100.0f, -100.0f) == 0.5f);
}

TEST_CASE("a rifle build maps onto the M16 at the rifle's reference stats") {
  const PartBuiltBase base = PartBuiltBaseFor(WeaponKind::kRifle);
  WeaponStats stats{
      .weight = 4.0f,
      .ergonomics = base.reference_ergonomics,
      .accuracy_moa = 3.0f,
      .sighting_range = 300.0f,
      .recoil_vertical = base.reference_recoil_vertical * 2.0f,
      .recoil_horizontal = base.reference_recoil_horizontal,
      .weapon_kind = WeaponKind::kRifle,
      .fire_modes = FireModeBit(FireMode::kSemi) | FireModeBit(FireMode::kAuto),
      .default_fire_mode = FireMode::kAuto,
      .fire_rate = 700.0f,
      .magazine_capacity = 20,
      .muzzle_velocity = 900.0f,
      .functional = true};
  const BuildTuning tuning = TuneFromStats(stats, "rifle");
  CHECK(tuning.spec.kind == WeaponKind::kRifle);
  CHECK(tuning.spec.rounds_per_minute == 700.0f);
  CHECK(tuning.spec.magazine_capacity == 20);
  CHECK(tuning.spec.muzzle_velocity == 900.0f);
  CHECK(tuning.viewmodel.muzzle_velocity == 900.0f);
  CHECK(tuning.spec.fire_mode == FireMode::kAuto);
  CHECK(tuning.spec.mass == 4.0f);
  CHECK(tuning.spec.bullet_mass == M16Spec().bullet_mass);
  CHECK(tuning.viewmodel.recoil_pitch_velocity ==
        doctest::Approx(2.0f * M16ViewmodelTuning().recoil_pitch_velocity));
  CHECK(tuning.viewmodel.look_kick_yaw == M16ViewmodelTuning().look_kick_yaw);
  CHECK(tuning.viewmodel.aim_time == M16ViewmodelTuning().aim_time);
  CHECK_FALSE(tuning.aim_fov_scale.has_value());
  // The rifle's own zero (50 m) lies inside the 25 .. 300 m range.
  CHECK(tuning.viewmodel.zero_distance == 50.0f);
}

TEST_CASE("missing figures keep the base, sights set the zero range") {
  const PartCatalog catalog = GlockCatalog();
  WeaponStats stats = PresetStats(catalog, "glock17");

  SUBCASE("no sights: a fixed zero") {
    stats.sighting_range = 0.0f;
    const BuildTuning tuning = TuneFromStats(stats, "x");
    CHECK_FALSE(tuning.zero_range.has_value());
    CHECK(tuning.viewmodel.zero_distance ==
          Glock17ViewmodelTuning().zero_distance);
  }
  SUBCASE("a red dot reaches further") {
    stats.sighting_range = 100.0f;
    const BuildTuning tuning = TuneFromStats(stats, "x");
    REQUIRE(tuning.zero_range.has_value());
    CHECK(tuning.zero_range->max == 100.0f);
    CHECK(tuning.viewmodel.zero_distance == 25.0f);
  }
  SUBCASE("sights shorter than the nearest zero") {
    stats.sighting_range = 15.0f;
    const BuildTuning tuning = TuneFromStats(stats, "x");
    REQUIRE(tuning.zero_range.has_value());
    CHECK(tuning.zero_range->min == 15.0f);
    CHECK(tuning.viewmodel.zero_distance == 15.0f);
  }
  SUBCASE("no barrel, no magazine, not functional") {
    stats.muzzle_velocity = 0.0f;
    stats.accuracy_moa = 0.0f;
    stats.magazine_capacity = 0;
    stats.functional = false;
    const BuildTuning tuning = TuneFromStats(stats, "x");
    CHECK(tuning.spec.muzzle_velocity == Glock17Spec().muzzle_velocity);
    CHECK(tuning.spec.magazine_capacity == 0);
    CHECK_FALSE(tuning.spec.functional);
    CHECK(tuning.accuracy_moa == 0.0f);
  }
}

TEST_CASE("zero ranges clamp and step") {
  const ZeroRange range{.min = 25.0f, .max = 100.0f, .step = 25.0f};
  CHECK(ClampZero(range, 10.0f) == 25.0f);
  CHECK(ClampZero(range, 300.0f) == 100.0f);
  CHECK(StepZero(range, 25.0f, 1) == 50.0f);
  CHECK(StepZero(range, 25.0f, -1) == 25.0f);
  CHECK(StepZero(range, 75.0f, 3) == 100.0f);
}

TEST_CASE("the readout summarises the stats and what is missing") {
  const PartCatalog catalog = GlockCatalog();
  const WeaponStats stats = PresetStats(catalog, "glock17");
  CHECK(StatsSummary(stats) ==
        "0.73 kg  ergo 91  4.4 MOA  50 m  recoil 302/252  1100 rpm");
  CHECK(MissingSummary(stats).empty());

  WeaponBuild bare(catalog, Id(catalog, "glock17_frame"));
  const WeaponStats bare_stats = ComputeStats(catalog, bare);
  CHECK(MissingSummary(bare_stats) == "NOT FUNCTIONAL: missing slide, barrel");
  CHECK(StatsSummary(bare_stats) ==
        "0.21 kg  ergo 90  - MOA  no sights  recoil 311/260  1100 rpm");
}

}  // namespace
}  // namespace blue::weapon::parts
