#include "game/weapon_builds.h"

#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "core/path.h"
#include "core/result.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/scratch_directory.h"
#include "tests/world_fixtures.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_geometry.h"
#include "weapon/weapon_item.h"
#include "world/prefab_cache.h"

namespace blue::game {
namespace {

namespace parts = weapon::parts;

constexpr std::string_view kCatalogText = R"([part frame]
category = lower_receiver
prefab = parts/frame.pbo
weight = 0.3
ergonomics = 91
weapon_kind = pistol
recoil_vertical = 311
recoil_horizontal = 260
fire_modes = semi
fire_rate = 1100
slot.slide = required slide

[part slide]
category = slide
prefab = parts/slide.pbo
weight = 0.3
slot.sight_rear = optional rear

[part rear]
category = sight_rear
prefab = parts/rear.pbo
sighting_range = 50

[part broken]
category = lower_receiver
prefab = parts/missing.pbo
fire_modes = semi
fire_rate = 600

[preset pistol]
name = Test pistol
root = frame
slide = slide
slide.sight_rear = rear

[part whole]
category = complete
fire_modes = semi
fire_rate = 60
legacy_type = spr_a3g

[preset broken]
root = broken

[preset whole]
root = whole
)";

prefab::Socket MakeSocket(std::string name, glm::vec3 position) {
  prefab::Socket socket;
  socket.name = std::move(name);
  socket.local.translation = position;
  return socket;
}

// An asset root with three cube parts and the catalogue above.
struct Assets {
  blue::testing::ScratchDirectory dir{"weapon_builds"};
  blue::testing::FakeGpu gpu;
  world::PrefabCache cache{gpu};
  weapon::WeaponLibrary library;

  Assets() {
    REQUIRE(platform::CreateDirectories(dir.Child("parts")));
    REQUIRE(platform::CreateDirectories(dir.Child("weapons/catalog")));
    prefab::Prefab frame = blue::testing::MakeCubePrefab({"steel"});
    frame.sockets = {MakeSocket("slot.slide", {0.0f, 0.5f, 0.0f}),
                     MakeSocket("grip", {0.0f, -0.3f, 0.0f})};
    prefab::Prefab slide = blue::testing::MakeCubePrefab({"steel"});
    slide.sockets = {MakeSocket("slot.sight_rear", {0.0f, 0.5f, -0.4f}),
                     MakeSocket("muzzle", {0.0f, 0.0f, 0.5f})};
    prefab::Prefab rear = blue::testing::MakeCubePrefab({"steel"});
    rear.sockets = {MakeSocket("sight", {0.0f, 0.1f, 0.0f})};
    REQUIRE(prefab::SavePrefab(frame, dir.Child("parts/frame.pbo")).ok());
    REQUIRE(prefab::SavePrefab(slide, dir.Child("parts/slide.pbo")).ok());
    REQUIRE(prefab::SavePrefab(rear, dir.Child("parts/rear.pbo")).ok());
    REQUIRE(platform::WriteFileAtomic(dir.Child("weapons/catalog/base.cfg"),
                                      std::as_bytes(std::span(kCatalogText)))
                .ok());
  }
};

TEST_CASE("without a catalogue there are only single-piece weapons") {
  blue::testing::ScratchDirectory dir("weapon_builds_none");
  blue::testing::FakeGpu gpu;
  world::PrefabCache cache(gpu);
  weapon::WeaponLibrary library;
  WeaponBuilds builds(cache, dir.path());
  CHECK_FALSE(builds.LoadCatalog());
  CHECK(builds.catalog() == nullptr);
  CHECK_FALSE(builds.PresetDef("pistol", library).has_value());
  CHECK_FALSE(builds.DefFor(parts::WeaponBuild(), library).ok());
  CHECK(library.size() == 0);
}

TEST_CASE("a broken catalogue file leaves no catalogue") {
  Assets assets;
  REQUIRE(platform::WriteFileAtomic(
              assets.dir.Child("weapons/catalog/zz.cfg"),
              std::as_bytes(std::span(std::string_view("[part x]\n?\n"))))
              .ok());
  WeaponBuilds builds(assets.cache, assets.dir.path());
  CHECK_FALSE(builds.LoadCatalog());
  CHECK_FALSE(builds.PresetDef("pistol", assets.library).has_value());
}

TEST_CASE("presets become shared build definitions") {
  Assets assets;
  WeaponBuilds builds(assets.cache, assets.dir.path());
  REQUIRE(builds.LoadCatalog());
  const parts::PartCatalog& catalog = *builds.catalog();

  const std::optional<weapon::WeaponDefId> pistol =
      builds.PresetDef("pistol", assets.library);
  REQUIRE(pistol.has_value());
  CHECK(assets.library.is_build(*pistol));
  const weapon::WeaponDef& def = assets.library.def(*pistol);
  CHECK(def.parts.size() == 3);
  // The name the firearm shows lives in the stored definition.
  CHECK(def.type.spec.display_name == "Test pistol");
  CHECK(def.type.spec.display_name.data() == def.name.data());
  CHECK(def.type.spec.functional);

  // The same build again is the same definition.
  CHECK(builds.PresetDef("pistol", assets.library) == pistol);
  const auto again = builds.DefFor(def.build, assets.library);
  REQUIRE(again.ok());
  CHECK(again.value() == *pistol);
  CHECK(assets.library.size() == 1);

  // Another build gets its own; the first stays where it is.
  parts::WeaponBuild bare = def.build;
  REQUIRE(parts::Remove(catalog, bare, "slide").ok());
  const auto other = builds.DefFor(bare, assets.library);
  REQUIRE(other.ok());
  CHECK(other.value() != *pistol);
  CHECK(&assets.library.def(*pistol) == &def);
  CHECK_FALSE(assets.library.def(other.value()).type.spec.functional);
  CHECK(assets.library.def(other.value()).missing_text ==
        "NOT FUNCTIONAL: missing slide");

  // A fresh weapon carries the build and the sights' zero.
  const weapon::WeaponItem item = weapon::MakeFreshWeapon(*pistol, def);
  CHECK(item.build == def.build);
  CHECK(item.scope_zero == 25.0f);
  CHECK(item.firearm.spec().display_name == "Test pistol");

  CHECK(builds.RebuildDefs(assets.library) == 2);
  CHECK(assets.library.def(*pistol).type.spec.display_name == "Test pistol");
}

TEST_CASE("a preset that cannot be built gives no definition") {
  Assets assets;
  WeaponBuilds builds(assets.cache, assets.dir.path());
  REQUIRE(builds.LoadCatalog());
  CHECK_FALSE(builds.PresetDef("broken", assets.library).has_value());
  CHECK_FALSE(builds.PresetDef("nope", assets.library).has_value());
  // A whole weapon without a model is left to its prefab.
  CHECK_FALSE(builds.PresetDef("whole", assets.library).has_value());
  CHECK(assets.library.size() == 0);
}

TEST_CASE("the shipped presets assemble from the cooked part prefabs") {
  blue::testing::FakeGpu gpu;
  world::PrefabCache cache(gpu);
  weapon::WeaponLibrary library;
  WeaponBuilds builds(cache, BLUE_ASSETS_DIR);
  REQUIRE(builds.LoadCatalog());

  const std::optional<weapon::WeaponDefId> glock =
      builds.PresetDef("glock17", library);
  REQUIRE(glock.has_value());
  const weapon::WeaponDef& built = library.def(*glock);
  CHECK(built.type.spec.functional);
  CHECK(built.type.spec.magazine_capacity == 17);
  CHECK(built.fire.has_value());
  CHECK(built.parts.size() == 6);  // frame, slide, 2 sights, barrel, mag

  // The parts land where the single-piece Glock's sockets are.
  auto whole = cache.Get(core::JoinPath(BLUE_ASSETS_DIR, "glock17.pbo"));
  REQUIRE(whole.ok());
  auto single = weapon::MakeWeaponDef(cache.prefab(whole.value()));
  REQUIRE(single.ok());
  REQUIRE(single.value().has_value());
  // The parts' bore is +Z (the part frame convention), the whole Glock's
  // -X: a quarter turn about +Y maps one onto the other.
  const auto turned = [](glm::vec3 v) { return glm::vec3(v.z, v.y, -v.x); };
  const weapon::WeaponGeometry& today = single.value()->geometry;
  const weapon::WeaponGeometry& from_parts = built.geometry;
  constexpr float kTolerance = 0.001f;  // m
  CHECK(glm::length(from_parts.muzzle - turned(today.muzzle)) < kTolerance);
  CHECK(glm::length(from_parts.sight_eye_point -
                    turned(today.sight_eye_point)) < kTolerance);
  CHECK(glm::dot(from_parts.sight_axis, turned(today.sight_axis)) > 0.9999f);
  CHECK(glm::length(from_parts.grip - turned(today.grip)) < kTolerance);
  CHECK(from_parts.optic_lens_radius == 0.0f);

  // The M16 is today's, the sniper rifle stays its prefab's.
  const std::optional<weapon::WeaponDefId> m16 =
      builds.PresetDef("m16", library);
  REQUIRE(m16.has_value());
  CHECK(library.def(*m16).type.id == "m16");
  CHECK(library.def(*m16).accuracy_moa == 0.0f);
  CHECK_FALSE(builds.PresetDef("spr_a3g", library).has_value());
}

}  // namespace
}  // namespace blue::game
