#include "weapon/weapon_def.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "core/path.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/part_catalog_samples.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/parts/weapon_tuning.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

using DefResult = core::Result<std::optional<WeaponDef>, std::string>;

prefab::Socket RootSocket(std::string name, glm::vec3 position) {
  prefab::Socket socket;
  socket.name = std::move(name);
  socket.local.translation = position;
  return socket;
}

// A pistol-like prefab in metres, bore along +Z: a "Slide" node, the three
// sockets and a 1 s fire clip whose slide is rearmost at 0.68 s. Names are
// capitalised like the user's Gen5 export.
prefab::Prefab MakePistolPrefab() {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "Pistol", .parent = -1, .rest = {}},
                            {.name = "Slide", .parent = 0, .rest = {}}};
  prefab.sockets = {RootSocket("Muzzle", {0.0f, 0.12f, 0.1f}),
                    RootSocket("Sight", {0.0f, 0.14f, -0.08f}),
                    RootSocket("GRIP", {0.0f, 0.04f, -0.07f})};
  animation::Clip fire{.name = "Fire", .duration = 1.0f};
  fire.channels.push_back({.node = 1,
                           .path = animation::TargetPath::kTranslation,
                           .times = {0.0f, 0.6f, 0.68f, 0.8f},
                           .values = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, -0.034f, 0.0f, 0.0f, 0.0f}});
  animation::AddEvent(fire, {.name = "Shot", .time = 0.6f});
  animation::AddEvent(fire, {.name = "Slide_Lock", .time = 0.68f});
  prefab.clips.push_back(std::move(fire));
  prefab.bounds = {.min = {-0.015f, 0.0f, -0.1f}, .max = {0.015f, 0.14f, 0.1f}};
  prefab::SetTag(prefab, kWeaponTag, "glock17");
  return prefab;
}

// The FN SPR A3G as imported (static, root node scaled 0.12, muzzle
// towards -Z) with the sockets measured on its mesh vertices, all in prefab
// space (node -1): the bore exit, the scope eyepiece's rear face with +Z
// down the scope axis (-Z of the model), the pistol grip and the centre of
// the eyepiece glass (the apex of its dome).
prefab::Prefab MakeSniperPrefab() {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {
      {.name = "spr_a3g", .parent = -1, .rest = {.scale = glm::vec3(0.12f)}},
      {.name = "spr_a3g.obj", .parent = 0, .rest = {}},
      {.name = "spr_a3g_Circle.034", .parent = 1, .rest = {}},
      {.name = "leupold_mark3_scope_Cylinder.020", .parent = 1, .rest = {}}};
  prefab::Socket sight = RootSocket("sight", {0.0f, 0.1821f, 0.2392f});
  sight.local.rotation = glm::quat(0.0f, 0.0f, 1.0f, 0.0f);  // 180 about +Y
  prefab.sockets = {RootSocket("muzzle", {0.0f, 0.1376f, -0.5698f}),
                    std::move(sight), RootSocket("grip", {0.0f, 0.06f, 0.275f}),
                    RootSocket("eyepiece", {0.0f, 0.1821f, 0.2332f})};
  prefab.bounds = {.min = {-0.0299f, 0.0029f, -0.5698f},
                   .max = {0.0600f, 0.2132f, 0.5783f}};
  prefab::SetTag(prefab, kWeaponTag, "spr_a3g");
  return prefab;
}

WeaponDef MakeDef(const prefab::Prefab& prefab) {
  DefResult made = MakeWeaponDef(prefab);
  REQUIRE(made.ok());
  REQUIRE(made.value().has_value());
  return std::move(*made.value());
}

std::string MakeError(const prefab::Prefab& prefab) {
  DefResult made = MakeWeaponDef(prefab);
  REQUIRE_FALSE(made.ok());
  return made.error();
}

prefab::Prefab LoadCookedWeapon(const char* name) {
  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(core::JoinPath(BLUE_ASSETS_DIR, name));
  REQUIRE(loaded.ok());
  return std::move(loaded).value();
}

TEST_CASE("Weapon types map ids to the code's specs") {
  const std::optional<WeaponType> glock = FindWeaponType("glock17");
  REQUIRE(glock.has_value());
  CHECK(glock->spec.kind == WeaponKind::kPistol);
  CHECK(std::string(glock->spec.display_name) ==
        std::string(Glock17Spec().display_name));
  CHECK(glock->viewmodel.eye_relief == Glock17ViewmodelTuning().eye_relief);
  CHECK(glock->aim_fov_scale == std::optional<float>(0.9f));
  const std::optional<WeaponType> m16 = FindWeaponType("m16");
  REQUIRE(m16.has_value());
  CHECK(m16->spec.kind == WeaponKind::kRifle);
  CHECK_FALSE(m16->aim_fov_scale.has_value());
  CHECK_FALSE(m16->scope.has_value());
  CHECK_FALSE(glock->scope.has_value());
  CHECK_FALSE(FindWeaponType("Glock17").has_value());
  CHECK(WeaponTypeIds().size() == 3);
}

TEST_CASE("The SPR A3G is a scoped bolt-action rifle") {
  const std::optional<WeaponType> spr = FindWeaponType("spr_a3g");
  REQUIRE(spr.has_value());
  const FirearmSpec& spec = spr->spec;
  CHECK(std::string(spec.display_name) == "FN SPR A3G");
  CHECK(spec.kind == WeaponKind::kRifle);
  CHECK(spec.magazine_capacity == 5);
  CHECK(spec.fire_modes == FireModeBit(FireMode::kSemi));
  CHECK(spec.fire_mode == FireMode::kSemi);
  CHECK(60.0f / spec.rounds_per_minute ==
        doctest::Approx(kSprA3GBoltCycleSeconds));
  CHECK(spec.mass == doctest::Approx(6.5f));
  CHECK(spr->viewmodel.muzzle_velocity == spec.muzzle_velocity);
  REQUIRE(spr->scope.has_value());
  CHECK(spr->scope->min_magnification == 3.5f);
  CHECK(spr->scope->max_magnification == 10.0f);

  // The bolt cycle limits trigger pulls: a second pull 1 s later does not
  // fire, one 1.2 s after the shot does.
  Firearm rifle(spec);
  CHECK(rifle.Update({.trigger_held = true}, 0.01f).shots == 1);
  CHECK(rifle.Update({}, 0.49f).shots == 0);
  CHECK(rifle.Update({.trigger_held = true}, 0.5f).shots == 0);
  CHECK(rifle.Update({}, 0.1f).shots == 0);
  CHECK(rifle.Update({.trigger_held = true}, 0.11f).shots == 1);
}

TEST_CASE("A tagged prefab with mixed-case names makes an animated weapon") {
  const WeaponDef def = MakeDef(MakePistolPrefab());
  CHECK(std::string(def.type.id) == "glock17");
  CHECK(def.geometry.scale == 1.0f);
  CHECK(glm::length(def.geometry.muzzle - glm::vec3(0.0f, 0.12f, 0.1f)) <
        1.0e-6f);
  CHECK(glm::length(def.geometry.grip - glm::vec3(0.0f, 0.04f, -0.07f)) <
        1.0e-6f);
  CHECK(def.geometry.sight_axis == glm::vec3(0.0f, 0.0f, 1.0f));
  CHECK(def.bounds.max.y == doctest::Approx(0.14f));
  REQUIRE(def.fire.has_value());
  CHECK(def.fire->clip.name == "Fire");
  CHECK(def.fire->tuning.fire_time == doctest::Approx(0.6f));
  CHECK(def.fire->tuning.lock_time == doctest::Approx(0.68f));
  CHECK(def.fire->hierarchy.nodes.size() == 2);
}

TEST_CASE("An untagged prefab is not a weapon, and that is no error") {
  prefab::Prefab prefab = MakePistolPrefab();
  REQUIRE(prefab::RemoveTag(prefab, kWeaponTag));
  const DefResult made = MakeWeaponDef(prefab);
  REQUIRE(made.ok());
  CHECK_FALSE(made.value().has_value());
}

TEST_CASE("Weapon definition errors name the problem") {
  prefab::Prefab prefab = MakePistolPrefab();
  SUBCASE("unknown type") {
    prefab::SetTag(prefab, kWeaponTag, "bazooka");
    const std::string error = MakeError(prefab);
    CHECK(error.find("bazooka") != std::string::npos);
    CHECK(error.find("glock17") != std::string::npos);  // The known ones.
  }
  SUBCASE("missing socket") {
    prefab.sockets.pop_back();
    CHECK(MakeError(prefab).find("'grip'") != std::string::npos);
  }
  SUBCASE("missing event") {
    prefab.clips[0].events.pop_back();
    CHECK(MakeError(prefab).find("slide_lock") != std::string::npos);
  }
}

TEST_CASE("A weapon without a fire clip is static") {
  prefab::Prefab prefab = MakePistolPrefab();
  prefab.clips.clear();
  prefab::SetTag(prefab, kWeaponTag, "m16");
  const WeaponDef def = MakeDef(prefab);
  CHECK_FALSE(def.fire.has_value());
  CHECK(def.type.spec.kind == WeaponKind::kRifle);
}

TEST_CASE("The SPR A3G prefab makes a static scoped weapon") {
  const WeaponDef def = MakeDef(MakeSniperPrefab());
  CHECK(std::string(def.type.id) == "spr_a3g");
  CHECK_FALSE(def.fire.has_value());  // Static: no fire clip.
  CHECK(def.type.scope.has_value());
  // The scope looks down-range (-Z), level with the bore, 4.45 cm above it.
  CHECK(glm::length(def.geometry.sight_axis - glm::vec3(0.0f, 0.0f, -1.0f)) <
        1.0e-6f);
  CHECK(def.geometry.sight_eye_point.y - def.geometry.muzzle.y ==
        doctest::Approx(0.0445f));
  CHECK(def.geometry.optic_lens_radius == 0.0f);  // No red dot.
  CHECK(glm::length(def.geometry.eyepiece - glm::vec3(0.0f, 0.1821f, 0.2332f)) <
        1.0e-6f);

  // Fully aimed and still, the scope axis is the view axis: the eyepiece
  // sits eye_relief ahead of the eye, the objective straight ahead of it,
  // and the zeroed bore rises a hair above the view to meet it at 100 m.
  Viewmodel viewmodel(def.geometry, def.type.viewmodel);
  ViewmodelInput input{.aim_held = true, .dt = 1.0f / 60.0f};
  for (int i = 0; i < 300; ++i) {
    viewmodel.Update(input);
  }
  REQUIRE(viewmodel.aim_fraction() == doctest::Approx(1.0f));
  const glm::mat4 model = viewmodel.model_matrix();
  const glm::vec3 eyepiece =
      glm::vec3(model * glm::vec4(def.geometry.sight_eye_point, 1.0f));
  const glm::vec3 objective =
      glm::vec3(model * glm::vec4(0.0f, 0.1821f, -0.1157f, 1.0f));
  CHECK(glm::length(eyepiece - glm::vec3(0.0f, 0.0f, -0.09f)) < 1.0e-4f);
  CHECK(std::abs(objective.x) < 1.0e-4f);
  CHECK(std::abs(objective.y) < 1.0e-4f);
  CHECK(objective.z == doctest::Approx(-0.09f - 0.3549f).epsilon(1.0e-3));
  const glm::vec3 bore = viewmodel.muzzle_direction();
  CHECK(bore.z < -0.999f);
  CHECK(std::abs(bore.x) < 1.0e-5f);
  CHECK(bore.y > 0.0f);  // The zero angle: under 2 mrad.
  CHECK(bore.y < 0.002f);
  // A bullet leaves with the shooter's eye straight above it.
  CHECK(std::abs(viewmodel.muzzle_position().x) < 1.0e-4f);
  CHECK(viewmodel.muzzle_position().y ==
        doctest::Approx(-0.0445f).epsilon(0.01));
}

TEST_CASE("Without an eyepiece socket the eyepiece is the sight point") {
  prefab::Prefab prefab = MakeSniperPrefab();
  prefab.sockets.pop_back();  // The eyepiece.
  const WeaponDef def = MakeDef(prefab);
  CHECK(def.geometry.eyepiece == def.geometry.sight_eye_point);
}

TEST_CASE("A fresh weapon is full and carries its definition") {
  const WeaponDef def = MakeDef(MakePistolPrefab());
  const WeaponItem item = MakeFreshWeapon(WeaponDefId{4}, def);
  CHECK(item.def == WeaponDefId{4});
  CHECK(item.firearm.rounds_in_magazine() == 17);
  CHECK(item.firearm.round_chambered());
  CHECK(item.firearm.spare_magazines().size() == 3);
  CHECK(item.scope_magnification == 0.0f);  // No scope.

  const WeaponItem sniper =
      MakeFreshWeapon(WeaponDefId{5}, MakeDef(MakeSniperPrefab()));
  CHECK(sniper.scope_magnification == 3.5f);
  CHECK(sniper.scope_zero == 100.0f);
  CHECK(item.scope_zero == 0.0f);
  CHECK(sniper.firearm.rounds_in_magazine() == 5);
}

TEST_CASE("The library keys definitions by prefab and keeps them stable") {
  WeaponLibrary library;
  const prefab::Prefab pistol = MakePistolPrefab();
  prefab::Prefab rifle = MakePistolPrefab();
  rifle.clips.clear();
  prefab::SetTag(rifle, kWeaponTag, "m16");
  prefab::Prefab prop = MakePistolPrefab();
  (void)prefab::RemoveTag(prop, kWeaponTag);

  const std::optional<WeaponDefId> a = library.Register(10, pistol, "a");
  REQUIRE(a.has_value());
  const WeaponDef* address = &library.def(*a);
  CHECK(library.Register(10, rifle, "a again") == a);  // Not re-read.
  CHECK(std::string(library.def(*a).type.id) == "glock17");
  CHECK_FALSE(library.Register(11, prop, "prop").has_value());
  const std::optional<WeaponDefId> b = library.Register(12, rifle, "b");
  REQUIRE(b.has_value());
  CHECK(*b != *a);
  CHECK(&library.def(*a) == address);  // Adding moved nothing.
  CHECK(library.size() == 2);
  CHECK(library.Find(12) == b);
  CHECK_FALSE(library.Find(11).has_value());
  CHECK(library.prefab_key(*b) == 12u);
  CHECK(library.FindType("m16") == b);
  CHECK_FALSE(library.FindType("nothing").has_value());

  // A two-pistol world: another glock17 prefab is its own definition.
  const std::optional<WeaponDefId> c = library.Register(13, pistol, "c");
  REQUIRE(c.has_value());
  CHECK(*c != *a);
  CHECK(library.FindType("glock17") == a);  // The first one.
}

TEST_CASE("Rebuilding keeps the previous data when the prefab breaks") {
  WeaponLibrary library;
  prefab::Prefab pistol = MakePistolPrefab();
  const std::optional<WeaponDefId> id = library.Register(1, pistol, "p");
  REQUIRE(id.has_value());

  // Fixed data: replaced in place.
  pistol.sockets[0].local.translation.z = 0.2f;
  CHECK(library.Rebuild(1, pistol, "p") == id);
  CHECK(library.def(*id).geometry.muzzle.z == doctest::Approx(0.2f));

  // Broken (missing socket) or untagged: the previous data stays.
  prefab::Prefab broken = pistol;
  broken.sockets.pop_back();
  CHECK(library.Rebuild(1, broken, "p") == id);
  CHECK(library.def(*id).geometry.muzzle.z == doctest::Approx(0.2f));
  prefab::Prefab untagged = pistol;
  (void)prefab::RemoveTag(untagged, kWeaponTag);
  CHECK(library.Rebuild(1, untagged, "p") == id);

  // A prefab that was not a weapon is looked at afresh.
  CHECK_FALSE(library.Register(2, untagged, "q").has_value());
  const std::optional<WeaponDefId> now = library.Rebuild(2, pistol, "q");
  REQUIRE(now.has_value());
  CHECK(library.size() == 2);
}

TEST_CASE("The cooked sample weapons are tagged weapon definitions") {
  const WeaponDef glock = MakeDef(LoadCookedWeapon("glock17.pbo"));
  CHECK(std::string(glock.type.id) == "glock17");
  CHECK(glock.type.spec.kind == WeaponKind::kPistol);
  REQUIRE(glock.fire.has_value());
  CHECK(glock.fire->tuning.fire_time == doctest::Approx(0.333f));
  CHECK(glock.fire->tuning.lock_time == doctest::Approx(0.4f));

  const WeaponDef m16 = MakeDef(LoadCookedWeapon("m16.pbo"));
  CHECK(std::string(m16.type.id) == "m16");
  CHECK(m16.type.spec.kind == WeaponKind::kRifle);
  CHECK_FALSE(m16.fire.has_value());
  CHECK(m16.geometry.optic_lens_radius > 0.0f);
}

// A part-built pistol definition: the test catalogue's stock Glock with
// a model of key `root_key`, named after the preset.
WeaponDef MakeBuildDef(uint32_t root_key, std::string name) {
  const parts::PartCatalog catalog = parts::testing::GlockCatalog();
  const parts::PresetDef* preset = catalog.FindPreset("glock17");
  REQUIRE(preset != nullptr);
  auto build = parts::MakeBuild(catalog, *preset);
  REQUIRE(build.ok());
  WeaponDef def = MakeDef(MakePistolPrefab());
  def.type.id = {};
  def.build = build.value();
  def.parts.front().prefab_key = root_key;
  def.name = std::move(name);
  def.zero_range = parts::ZeroRange{.min = 25.0f, .max = 50.0f, .step = 25.0f};
  def.switch_time_scale = 1.2f;
  return def;
}

TEST_CASE("The library keeps one definition per build and root model") {
  WeaponLibrary library;
  const std::optional<WeaponDefId> legacy =
      library.Register(3, MakePistolPrefab(), "legacy");
  REQUIRE(legacy.has_value());
  CHECK_FALSE(library.is_build(*legacy));
  CHECK(library.def(*legacy).parts.size() == 1);
  CHECK(library.def(*legacy).parts[0].prefab_key == 3u);

  WeaponDef def = MakeBuildDef(7, "Built pistol");
  const parts::WeaponBuild build = def.build;
  const WeaponDefId id = library.AddBuild(std::move(def));
  CHECK(library.is_build(id));
  CHECK(library.prefab_key(id) == 7u);
  // The name the firearm shows views the stored definition's.
  const WeaponDef& stored = library.def(id);
  CHECK(stored.type.spec.display_name == "Built pistol");
  CHECK(stored.type.spec.display_name.data() == stored.name.data());
  CHECK(library.FindBuild(build, 7) == id);
  CHECK_FALSE(library.FindBuild(build, 8).has_value());
  CHECK_FALSE(library.FindBuild(parts::WeaponBuild(), 7).has_value());
  // A build is not a prefab key.
  CHECK_FALSE(library.Find(7).has_value());

  WeaponDef replacement = MakeBuildDef(7, "Renamed");
  replacement.accuracy_moa = 3.0f;
  library.ReplaceBuild(id, std::move(replacement));
  CHECK(&library.def(id) == &stored);
  CHECK(stored.accuracy_moa == 3.0f);
  CHECK(stored.type.spec.display_name == "Renamed");
  CHECK(stored.type.spec.display_name.data() == stored.name.data());
}

TEST_CASE("RefitWeapon moves a weapon to its new build's definition") {
  WeaponLibrary library;
  const WeaponDefId stock = library.AddBuild(MakeBuildDef(7, "Pistol"));
  WeaponItem item = MakeFreshWeapon(stock, library.def(stock));
  CHECK(item.build == library.def(stock).build);
  CHECK(item.switch_time_scale == 1.2f);
  CHECK(item.scope_zero == 25.0f);  // The tuning's zero, in range.
  item.scope_zero = 50.0f;

  WeaponDef extended = MakeBuildDef(7, "Pistol");
  extended.type.spec.magazine_capacity = 33;
  extended.zero_range =
      parts::ZeroRange{.min = 25.0f, .max = 100.0f, .step = 25.0f};
  extended.switch_time_scale = 1.5f;
  const WeaponDefId bigger = library.AddBuild(std::move(extended));
  RefitWeapon(item, bigger, library.def(bigger));
  CHECK(item.def == bigger);
  CHECK(item.firearm.spec().magazine_capacity == 33);
  CHECK(item.firearm.rounds_in_magazine() == 17);
  CHECK(item.switch_time_scale == 1.5f);
  CHECK(item.scope_zero == 50.0f);  // Still within the range.

  WeaponDef no_sights = MakeBuildDef(7, "Pistol");
  no_sights.zero_range.reset();
  const WeaponDef& fixed = library.def(library.AddBuild(std::move(no_sights)));
  RefitWeapon(item, WeaponDefId{2}, fixed);
  CHECK(item.scope_zero == 0.0f);
  CHECK(item.scope_magnification == 0.0f);

  // Sights again: the zero starts at the tuning's.
  RefitWeapon(item, stock, library.def(stock));
  CHECK(item.scope_zero == 25.0f);
}

}  // namespace
}  // namespace blue::weapon
