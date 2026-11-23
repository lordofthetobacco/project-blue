#include "weapon/parts/weapon_assembly.h"

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/transform.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/part_catalog_file.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_animator.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon::parts {
namespace {

// A pistol whose parts all have models (paths are keys of Models below),
// a part without one, a single-piece rifle and a frame lacking a socket.
constexpr std::string_view kCatalogText = R"(
[part frame]
name = Test frame
category = lower_receiver
prefab = frame.pbo
weight = 0.2
ergonomics = 90
weapon_kind = pistol
recoil_vertical = 300
recoil_horizontal = 250
fire_modes = double_action, semi
fire_rate = 1000
slot.slide = required slide
slot.barrel = required barrel, barrel_threaded
slot.magazine = optional mag
slot.light = optional light

[part slide]
category = slide
prefab = slide.pbo
weight = 0.25
slot.sight_rear = optional rear, dot

[part rear]
category = sight_rear
prefab = rear.pbo
sighting_range = 50

[part dot]
category = optic
prefab = dot.pbo
sighting_range = 100

[part barrel]
category = barrel
prefab = barrel.pbo
accuracy_moa = 4
muzzle_velocity = 360

[part barrel_threaded]
category = barrel
prefab = threaded.pbo
accuracy_moa = 4
muzzle_velocity = 360
slot.muzzle = optional can

[part can]
category = muzzle
prefab = can.pbo
recoil = -10
velocity = 0.98

[part mag]
category = magazine
prefab = mag.pbo
capacity = 15

[part light]
category = tactical

[part bare_frame]
category = lower_receiver
prefab = bare_frame.pbo
fire_modes = semi
fire_rate = 600
slot.slide = required slide

[part rifle]
name = Test rifle
category = complete
prefab = rifle.pbo
weapon_kind = rifle
fire_modes = semi, auto
fire_rate = 800
legacy_type = m16
accuracy_moa = 3
capacity = 30

[part ghost]
category = complete
fire_modes = semi
fire_rate = 600

[preset pistol]
name = Test pistol
root = frame
slide = slide
slide.sight_rear = rear
barrel = barrel
magazine = mag

[preset rifle]
root = rifle
)";

PartCatalog Catalog() {
  auto file = ParsePartCatalog(kCatalogText, "test.cfg");
  REQUIRE_MESSAGE(file.ok(), file.error().ToString());
  const std::array<PartCatalogFile, 1> files = {std::move(file).value()};
  auto catalog = MergePartCatalogs(files);
  REQUIRE_MESSAGE(catalog.ok(), catalog.error().ToString());
  return std::move(catalog).value();
}

PartId Id(const PartCatalog& catalog, std::string_view id) {
  const std::optional<PartId> part = catalog.Find(id);
  REQUIRE_MESSAGE(part.has_value(), id);
  return *part;
}

prefab::Socket MakeSocket(std::string name, int node, glm::vec3 position,
                          glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f,
                                                         0.0f)) {
  prefab::Socket socket;
  socket.name = std::move(name);
  socket.node = node;
  socket.local.translation = position;
  socket.local.rotation = rotation;
  return socket;
}

// A prefab with one mesh entry (bounds only; no vertex data is needed).
prefab::Prefab WithBounds(prefab::Prefab prefab, glm::vec3 min, glm::vec3 max) {
  prefab.bounds = {.min = min, .max = max};
  prefab.meshes.push_back(
      {.name = "mesh", .node = -1, .bounds = prefab.bounds});
  return prefab;
}

// The frame: a "Slide" node 0.1 up and a "Barrel" node, both moved by a
// 1 s fire clip (the slide back 0.034 at 0.68 s), slot sockets on them,
// and fallback muzzle / sight / grip sockets at the root. Bore along +Z.
prefab::Prefab MakeFrame(bool with_slot_sockets = true) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "Root", .parent = -1, .rest = {}},
                            {.name = "Slide",
                             .parent = 0,
                             .rest = {.translation = {0.0f, 0.1f, 0.0f}}},
                            {.name = "Barrel",
                             .parent = 0,
                             .rest = {.translation = {0.0f, 0.08f, 0.02f}}}};
  if (with_slot_sockets) {
    prefab.sockets = {
        MakeSocket("slot.slide", 1, {0.0f, 0.0f, -0.1f}),
        MakeSocket("Slot.Barrel", 2, {0.0f, 0.0f, 0.0f}),
        MakeSocket("slot.magazine", -1, {0.0f, -0.02f, -0.06f}),
        MakeSocket("slot.light", -1, {0.0f, 0.05f, 0.08f}),
    };
  }
  prefab.sockets.push_back(MakeSocket("grip", -1, {0.0f, -0.03f, -0.07f}));
  prefab.sockets.push_back(MakeSocket("muzzle", -1, {0.0f, 0.08f, 0.5f}));
  prefab.sockets.push_back(MakeSocket("sight", -1, {0.0f, 0.2f, -0.5f}));
  animation::Clip fire{.name = "fire", .duration = 1.0f};
  for (const int node : {1, 2}) {
    const glm::vec3 rest = prefab.hierarchy.nodes[node].rest.translation;
    fire.channels.push_back(
        {.node = node,
         .path = animation::TargetPath::kTranslation,
         .times = {0.0f, 0.6f, 0.68f, 0.8f},
         .values = {rest.x, rest.y, rest.z, rest.x, rest.y, rest.z, rest.x,
                    rest.y, rest.z - 0.034f, rest.x, rest.y, rest.z}});
  }
  animation::AddEvent(fire, {.name = "shot", .time = 0.6f});
  animation::AddEvent(fire, {.name = "slide_lock", .time = 0.68f});
  prefab.clips.push_back(std::move(fire));
  return WithBounds(std::move(prefab), {-0.015f, -0.12f, -0.1f},
                    {0.015f, 0.08f, 0.1f});
}

// Every model of the catalogue by path, and the ModelSource over them.
struct Models {
  std::map<std::string, prefab::Prefab, std::less<>> prefabs;

  Models() {
    prefabs["frame.pbo"] = MakeFrame();
    prefabs["bare_frame.pbo"] = MakeFrame(/*with_slot_sockets=*/false);
    prefabs["bare_frame.pbo"].sockets.clear();
    {
      prefab::Prefab slide;
      slide.sockets = {MakeSocket("slot.sight_rear", -1, {0.0f, 0.02f, 0.01f}),
                       MakeSocket("sight", -1, {0.0f, 0.03f, 0.0f})};
      prefabs["slide.pbo"] = WithBounds(std::move(slide), {-0.01f, -0.02f, 0},
                                        {0.01f, 0.02f, 0.19f});
    }
    {
      prefab::Prefab rear;
      rear.sockets = {MakeSocket("sight", -1, {0.0f, 0.005f, 0.0f})};
      prefabs["rear.pbo"] = WithBounds(std::move(rear), {-0.007f, 0, -0.002f},
                                       {0.007f, 0.005f, 0.002f});
    }
    {
      // The lens socket's X scale is the lens radius.
      prefab::Prefab dot;
      dot.sockets = {MakeSocket("sight", -1, {0.0f, 0.016f, -0.01f}),
                     MakeSocket("lens", -1, {0.0f, 0.016f, 0.03f})};
      dot.sockets.back().local.scale = glm::vec3(0.009f);
      prefabs["dot.pbo"] = WithBounds(std::move(dot), {-0.013f, 0, -0.011f},
                                      {0.013f, 0.028f, 0.034f});
    }
    {
      prefab::Prefab barrel;
      barrel.sockets = {MakeSocket("muzzle", -1, {0.0f, 0.0f, 0.117f})};
      prefabs["barrel.pbo"] = WithBounds(std::move(barrel), {-0.01f, -0.01f, 0},
                                         {0.01f, 0.01f, 0.117f});
    }
    {
      prefab::Prefab threaded;
      threaded.sockets = {MakeSocket("muzzle", -1, {0.0f, 0.0f, 0.129f}),
                          MakeSocket("slot.muzzle", -1, {0.0f, 0.0f, 0.117f})};
      prefabs["threaded.pbo"] = WithBounds(
          std::move(threaded), {-0.01f, -0.01f, 0}, {0.01f, 0.01f, 0.129f});
    }
    {
      // Mounted 1 cm into its rear face, turned a quarter about the bore.
      prefab::Prefab can;
      can.sockets = {MakeSocket("mount", -1, {0.0f, 0.0f, 0.01f},
                                glm::angleAxis(glm::radians(90.0f),
                                               glm::vec3(0.0f, 0.0f, 1.0f))),
                     MakeSocket("muzzle", -1, {0.0f, 0.0f, 0.165f})};
      prefabs["can.pbo"] = WithBounds(std::move(can), {-0.017f, -0.017f, 0},
                                      {0.017f, 0.017f, 0.165f});
    }
    {
      prefab::Prefab mag;
      prefabs["mag.pbo"] = WithBounds(
          std::move(mag), {-0.011f, -0.12f, -0.015f}, {0.011f, 0.005f, 0.058f});
    }
    {
      prefab::Prefab rifle;
      rifle.sockets = {MakeSocket("muzzle", -1, {0.0f, 0.06f, 0.9f}),
                       MakeSocket("sight", -1, {0.0f, 0.1f, -0.1f}),
                       MakeSocket("grip", -1, {0.0f, -0.05f, -0.15f})};
      prefabs["rifle.pbo"] = WithBounds(
          std::move(rifle), {-0.03f, -0.2f, -0.5f}, {0.03f, 0.12f, 0.9f});
    }
  }

  // Keys are the position of the path in the map, plus 100.
  [[nodiscard]] ModelSource Source() const {
    return [this](std::string_view path) {
      using ModelResult = core::Result<PartModel, std::string>;
      const auto found = prefabs.find(path);
      if (found == prefabs.end()) {
        return ModelResult::Error("no such file");
      }
      return ModelResult::Ok({.key = Key(path), .prefab = &found->second});
    };
  }

  [[nodiscard]] uint32_t Key(std::string_view path) const {
    uint32_t key = 100;
    for (const auto& [name, prefab] : prefabs) {
      if (name == path) {
        return key;
      }
      ++key;
    }
    return 0;
  }
};

WeaponBuild PresetBuild(const PartCatalog& catalog, std::string_view id) {
  const PresetDef* preset = catalog.FindPreset(id);
  REQUIRE(preset != nullptr);
  core::Result<WeaponBuild, BuildError> build = MakeBuild(catalog, *preset);
  REQUIRE_MESSAGE(build.ok(), build.error().message);
  return std::move(build).value();
}

void InstallPart(const PartCatalog& catalog, WeaponBuild& build,
                 std::string_view path, std::string_view part) {
  const auto replaced =
      Install(catalog, build, path, WeaponBuild(catalog, Id(catalog, part)));
  REQUIRE_MESSAGE(replaced.ok(), replaced.error().message);
}

WeaponDef Assemble(const PartCatalog& catalog, const WeaponBuild& build,
                   const Models& models) {
  core::Result<WeaponDef, std::string> def =
      AssembleWeapon(catalog, build, models.Source());
  REQUIRE_MESSAGE(def.ok(), def.error());
  return std::move(def).value();
}

glm::vec3 Point(const glm::mat4& matrix, glm::vec3 point = glm::vec3(0.0f)) {
  return glm::vec3(matrix * glm::vec4(point, 1.0f));
}

void CheckNear(glm::vec3 actual, glm::vec3 expected) {
  CHECK(actual.x == doctest::Approx(expected.x));
  CHECK(actual.y == doctest::Approx(expected.y));
  CHECK(actual.z == doctest::Approx(expected.z));
}

void CheckSameMatrix(const glm::mat4& a, const glm::mat4& b) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      CHECK(a[column][row] == doctest::Approx(b[column][row]));
    }
  }
}

TEST_CASE("parts hang on their parent's slot sockets, nested ones too") {
  const PartCatalog catalog = Catalog();
  const Models models;
  const WeaponDef def =
      Assemble(catalog, PresetBuild(catalog, "pistol"), models);
  // Root, slide, the rear sight on it, barrel, magazine: depth first.
  REQUIRE(def.parts.size() == 5);
  const std::array<std::string_view, 5> ids = {"frame", "slide", "rear",
                                               "barrel", "mag"};
  const std::array<int, 5> parents = {-1, 0, 1, 0, 0};
  const std::array<int, 5> nodes = {-1, 1, -1, 2, -1};
  for (size_t i = 0; i < def.parts.size(); ++i) {
    CAPTURE(i);
    CHECK(def.parts[i].part == Id(catalog, ids[i]));
    CHECK(def.parts[i].parent == parents[i]);
    CHECK(def.parts[i].parent_node == nodes[i]);
  }
  CHECK(def.parts[0].prefab_key == models.Key("frame.pbo"));
  CHECK(def.parts[2].prefab_key == models.Key("rear.pbo"));

  // The slide sits on its socket on the Slide node (no mount socket).
  const PartPlacement& slide = def.parts[1];
  CheckNear(Point(slide.local), {0.0f, 0.0f, -0.1f});
  CHECK(slide.root_node == 1);
  CheckSameMatrix(slide.root_local, slide.local);
  CheckNear(Point(slide.rest), {0.0f, 0.1f, -0.1f});

  // The rear sight on the slide follows the slide's node.
  const PartPlacement& rear = def.parts[2];
  CHECK(rear.root_node == 1);
  CheckNear(Point(rear.local), {0.0f, 0.02f, 0.01f});
  CheckNear(Point(rear.root_local), {0.0f, 0.02f, -0.09f});
  CheckNear(Point(rear.rest), {0.0f, 0.12f, -0.09f});

  // The magazine hangs on the root.
  CHECK(def.parts[4].root_node == -1);
  CheckNear(Point(def.parts[4].rest), {0.0f, -0.02f, -0.06f});
}

TEST_CASE("parts on animated nodes follow the fire animation") {
  const PartCatalog catalog = Catalog();
  const Models models;
  WeaponBuild build = PresetBuild(catalog, "pistol");
  InstallPart(catalog, build, "barrel", "barrel_threaded");
  InstallPart(catalog, build, "barrel.muzzle", "can");
  const WeaponDef def = Assemble(catalog, build, models);
  REQUIRE(def.fire.has_value());
  CHECK(def.fire->tuning.fire_time == doctest::Approx(0.6f));

  WeaponAnimator animator(&def.fire->hierarchy, &def.fire->clip,
                          def.fire->tuning);
  std::vector<glm::mat4> world(def.fire->hierarchy.nodes.size());
  animator.Update(1, false, 0.0f);  // The shot restarts the clip at 0.6 s.
  animator.Update(0, false, 0.05f);
  animator.ComputeWorld(glm::mat4(1.0f), world);
  // The nodes are back by the clip's key from 0.6 s (rest) to 0.68 s.
  const float t = animator.time();
  REQUIRE(t >= 0.6f);
  REQUIRE(t <= 0.68f);
  const float back = 0.034f * (t - 0.6f) / 0.08f;
  REQUIRE(back > 0.0f);
  const auto posed = [&](const PartPlacement& part) {
    const glm::mat4 anchor = part.root_node < 0
                                 ? glm::mat4(1.0f)
                                 : world[static_cast<size_t>(part.root_node)];
    return Point(anchor * part.root_local);
  };
  // Slide, the sight on it, the barrel and the can on the barrel all move
  // back with their nodes; the magazine on the root stays.
  for (size_t i = 1; i < def.parts.size(); ++i) {
    CAPTURE(i);
    const glm::vec3 moved = posed(def.parts[i]) - Point(def.parts[i].rest);
    const bool on_root = def.parts[i].root_node < 0;
    CheckNear(moved, on_root ? glm::vec3(0.0f) : glm::vec3(0.0f, 0.0f, -back));
  }
}

TEST_CASE("a mount socket lands on the slot socket") {
  const PartCatalog catalog = Catalog();
  const Models models;
  WeaponBuild build = PresetBuild(catalog, "pistol");
  InstallPart(catalog, build, "barrel", "barrel_threaded");
  InstallPart(catalog, build, "barrel.muzzle", "can");
  const WeaponDef def = Assemble(catalog, build, models);
  REQUIRE(def.parts.size() == 6);
  const PartPlacement& barrel = def.parts[3];
  const PartPlacement& can = def.parts[4];
  CHECK(can.part == Id(catalog, "can"));
  CHECK(can.parent == 3);
  CHECK(can.root_node == 2);  // The barrel's node.
  // The can's mount (1 cm in, turned 90 degrees) meets the slot exactly:
  // slot x mount^-1 x mount = slot.
  const glm::mat4 mount =
      animation::ToMatrix(models.prefabs.at("can.pbo").sockets[0].local);
  const glm::mat4 slot =
      animation::ToMatrix(models.prefabs.at("threaded.pbo").sockets[1].local);
  CheckSameMatrix(can.local * mount, slot);
  CheckSameMatrix(can.rest * mount, barrel.rest * slot);
  // The can's tip is 0.155 past the slot, which is 0.117 down the barrel.
  CheckNear(Point(can.rest, {0.0f, 0.0f, 0.165f}),
            Point(barrel.rest, {0.0f, 0.0f, 0.117f + 0.155f}));
}

TEST_CASE("functional sockets come from the parts in contract order") {
  const PartCatalog catalog = Catalog();
  const Models models;
  WeaponBuild build = PresetBuild(catalog, "pistol");
  const glm::vec3 barrel_origin(0.0f, 0.08f, 0.02f);

  SUBCASE("stock: barrel muzzle, rear sight, frame grip, no optic") {
    const WeaponDef def = Assemble(catalog, build, models);
    CHECK(def.geometry.scale == 1.0f);
    CheckNear(def.geometry.muzzle, barrel_origin + glm::vec3(0, 0, 0.117f));
    // Rear sight at the slide's (0, 0.02, 0.01) on the Slide socket.
    CheckNear(def.geometry.sight_eye_point, {0.0f, 0.125f, -0.09f});
    CheckNear(def.geometry.sight_axis, {0.0f, 0.0f, 1.0f});
    CheckNear(def.geometry.grip, {0.0f, -0.03f, -0.07f});
    CHECK(def.geometry.optic_lens_radius == 0.0f);
    CheckNear(def.geometry.optic_lens, def.geometry.sight_eye_point);
    CheckNear(def.geometry.eyepiece, def.geometry.sight_eye_point);
  }
  SUBCASE("a muzzle device gives the muzzle") {
    InstallPart(catalog, build, "barrel", "barrel_threaded");
    InstallPart(catalog, build, "barrel.muzzle", "can");
    const WeaponDef def = Assemble(catalog, build, models);
    CheckNear(def.geometry.muzzle,
              barrel_origin + glm::vec3(0, 0, 0.117f + 0.155f));
  }
  SUBCASE("an optic gives the sight and the lens") {
    InstallPart(catalog, build, "slide.sight_rear", "dot");
    const WeaponDef def = Assemble(catalog, build, models);
    CheckNear(def.geometry.sight_eye_point, {0.0f, 0.136f, -0.1f});
    CheckNear(def.geometry.optic_lens, {0.0f, 0.136f, -0.06f});
    CHECK(def.geometry.optic_lens_radius == doctest::Approx(0.009f));
  }
  SUBCASE("without a sight part the slide's sight is used") {
    REQUIRE(Remove(catalog, build, "slide.sight_rear").ok());
    const WeaponDef def = Assemble(catalog, build, models);
    CheckNear(def.geometry.sight_eye_point, {0.0f, 0.13f, -0.1f});
  }
  SUBCASE("without slide and barrel the root's sockets stand in") {
    REQUIRE(Remove(catalog, build, "slide").ok());
    REQUIRE(Remove(catalog, build, "barrel").ok());
    const WeaponDef def = Assemble(catalog, build, models);
    CheckNear(def.geometry.muzzle, {0.0f, 0.08f, 0.5f});
    CheckNear(def.geometry.sight_eye_point, {0.0f, 0.2f, -0.5f});
  }
}

TEST_CASE("missing functional sockets fall back without failing") {
  const PartCatalog catalog = Catalog();
  const Models models;
  const WeaponBuild build(catalog, Id(catalog, "bare_frame"));
  const WeaponDef def = Assemble(catalog, build, models);
  CheckNear(def.geometry.sight_eye_point, glm::vec3(0.0f));
  CheckNear(def.geometry.sight_axis, {0.0f, 0.0f, 1.0f});
  CheckNear(def.geometry.muzzle, glm::vec3(0.0f));
  CheckNear(def.geometry.grip, glm::vec3(0.0f));
  CHECK_FALSE(def.type.spec.functional);
  CHECK(def.missing_text == "NOT FUNCTIONAL: missing slide");
}

TEST_CASE("bounds take in every part at rest") {
  const PartCatalog catalog = Catalog();
  const Models models;
  WeaponBuild build = PresetBuild(catalog, "pistol");
  InstallPart(catalog, build, "barrel", "barrel_threaded");
  InstallPart(catalog, build, "barrel.muzzle", "can");
  const WeaponDef def = Assemble(catalog, build, models);
  // The frame's box, the magazine's bottom and the can's tip.
  CHECK(def.bounds.min.x == doctest::Approx(-0.017f));
  CHECK(def.bounds.min.y == doctest::Approx(-0.14f));
  CHECK(def.bounds.max.z == doctest::Approx(0.02f + 0.117f + 0.155f));
  CHECK(def.bounds.max.y == doctest::Approx(0.125f));  // The rear sight.
}

TEST_CASE("the build's stats make its spec, feel and readout") {
  const PartCatalog catalog = Catalog();
  const Models models;
  WeaponBuild build = PresetBuild(catalog, "pistol");
  const WeaponDef stock = Assemble(catalog, build, models);
  CHECK(stock.name == "Test pistol");
  CHECK(stock.build == build);
  REQUIRE(stock.stats.has_value());
  CHECK(stock.stats->functional);
  CHECK(stock.type.spec.functional);
  CHECK(stock.type.spec.magazine_capacity == 15);
  CHECK(stock.type.spec.rounds_per_minute == 1000.0f);
  CHECK(stock.type.spec.fire_mode == FireMode::kDoubleAction);
  CHECK(stock.type.spec.muzzle_velocity == 360.0f);
  CHECK(stock.type.spec.kind == WeaponKind::kPistol);
  CHECK(stock.type.viewmodel.eye_relief == Glock17ViewmodelTuning().eye_relief);
  CHECK(stock.accuracy_moa == 4.0f);
  REQUIRE(stock.zero_range.has_value());
  CHECK(stock.zero_range->max == 50.0f);
  CHECK(stock.missing_text.empty());
  CHECK_FALSE(stock.stats_text.empty());
  // The view of the name is set by the library, where the def stays put.
  CHECK(stock.type.spec.display_name.empty());

  InstallPart(catalog, build, "slide.sight_rear", "dot");
  REQUIRE(Remove(catalog, build, "magazine").ok());
  const WeaponDef changed = Assemble(catalog, build, models);
  CHECK(changed.name == "Test pistol");  // The preset on the same root.
  CHECK(changed.type.spec.magazine_capacity == 0);
  REQUIRE(changed.zero_range.has_value());
  CHECK(changed.zero_range->max == 100.0f);
}

TEST_CASE("a part without a model is listed and draws nothing") {
  const PartCatalog catalog = Catalog();
  const Models models;
  WeaponBuild build = PresetBuild(catalog, "pistol");
  InstallPart(catalog, build, "light", "light");
  const WeaponDef def = Assemble(catalog, build, models);
  REQUIRE(def.parts.size() == 6);
  CHECK(def.parts[5].part == Id(catalog, "light"));
  CHECK_FALSE(def.parts[5].prefab_key.has_value());
  CheckNear(Point(def.parts[5].rest), {0.0f, 0.05f, 0.08f});
}

TEST_CASE("a complete part with a legacy type is today's weapon exactly") {
  const PartCatalog catalog = Catalog();
  const Models models;
  const WeaponBuild build = PresetBuild(catalog, "rifle");
  const WeaponDef def = Assemble(catalog, build, models);

  prefab::Prefab tagged = models.prefabs.at("rifle.pbo");
  prefab::SetTag(tagged, kWeaponTag, "m16");
  auto legacy = MakeWeaponDef(tagged);
  REQUIRE(legacy.ok());
  REQUIRE(legacy.value().has_value());
  const WeaponDef& today = *legacy.value();

  CHECK(def.type.id == "m16");
  CHECK(std::string(def.type.spec.display_name) ==
        std::string(M16Spec().display_name));
  CHECK(def.type.spec.magazine_capacity == today.type.spec.magazine_capacity);
  CHECK(def.type.spec.mass == today.type.spec.mass);
  CHECK(def.type.spec.fire_modes == today.type.spec.fire_modes);
  CHECK(def.type.viewmodel.look_kick_pitch ==
        today.type.viewmodel.look_kick_pitch);
  CHECK(def.type.viewmodel.aim_time == today.type.viewmodel.aim_time);
  CHECK(def.geometry.muzzle == today.geometry.muzzle);
  CHECK(def.geometry.sight_eye_point == today.geometry.sight_eye_point);
  CHECK(def.geometry.sight_axis == today.geometry.sight_axis);
  CHECK(def.geometry.grip == today.geometry.grip);
  CHECK(def.bounds.min == today.bounds.min);
  CHECK(def.bounds.max == today.bounds.max);
  // No dispersion, no zero keys, normal switch times: behaviour as today.
  CHECK(def.accuracy_moa == 0.0f);
  CHECK_FALSE(def.zero_range.has_value());
  CHECK(def.switch_time_scale == 1.0f);
  CHECK(def.name.empty());
  // But it knows its parts and stats.
  CHECK(def.build == build);
  REQUIRE(def.parts.size() == 1);
  CHECK(def.parts[0].prefab_key == models.Key("rifle.pbo"));
  REQUIRE(def.stats.has_value());
  CHECK(def.stats->magazine_capacity == 30);
}

TEST_CASE("assembly errors name the part") {
  const PartCatalog catalog = Catalog();
  Models models;
  SUBCASE("a missing slot socket") {
    WeaponBuild build(catalog, Id(catalog, "bare_frame"));
    InstallPart(catalog, build, "slide", "slide");
    const auto def = AssembleWeapon(catalog, build, models.Source());
    REQUIRE_FALSE(def.ok());
    CHECK(def.error().find("'bare_frame'") != std::string::npos);
    CHECK(def.error().find("no 'slot.slide' socket") != std::string::npos);
  }
  SUBCASE("a model that cannot be loaded") {
    models.prefabs.erase("mag.pbo");
    const auto def = AssembleWeapon(catalog, PresetBuild(catalog, "pistol"),
                                    models.Source());
    REQUIRE_FALSE(def.ok());
    CHECK(def.error() == "part 'mag': cannot load mag.pbo: no such file");
  }
  SUBCASE("a root without a model") {
    const WeaponBuild build(catalog, Id(catalog, "ghost"));
    const auto def = AssembleWeapon(catalog, build, models.Source());
    REQUIRE_FALSE(def.ok());
    CHECK(def.error().find("'ghost' has no model") != std::string::npos);
  }
  SUBCASE("an empty build") {
    CHECK_FALSE(AssembleWeapon(catalog, WeaponBuild(), models.Source()).ok());
  }
}

TEST_CASE("a build is named after its preset, else its root") {
  const PartCatalog catalog = Catalog();
  CHECK(BuildName(catalog, PresetBuild(catalog, "pistol")) == "Test pistol");
  CHECK(BuildName(catalog, WeaponBuild(catalog, Id(catalog, "frame"))) ==
        "Test pistol");
  CHECK(BuildName(catalog, WeaponBuild(catalog, Id(catalog, "bare_frame"))) ==
        "bare_frame");
  CHECK(BuildName(catalog, WeaponBuild()).empty());
}

}  // namespace
}  // namespace blue::weapon::parts
