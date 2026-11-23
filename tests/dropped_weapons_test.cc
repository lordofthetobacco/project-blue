#include "weapon/dropped_weapons.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "tests/part_catalog_samples.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_specs.h"
#include "weapon/weapon_throw.h"

namespace blue::weapon {
namespace {

constexpr float kStep = 1.0f / 60.0f;

// A world with gravity and a static ground whose surface is y = 0.
struct Range {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime};

  Range() {
    world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    AddGround();
  }

  void AddGround() {
    physics::BodyDesc ground;
    ground.body.shape = scene::ShapeType::kBox;
    ground.body.motion = scene::MotionType::kStatic;
    ground.bounds = {.min = {-20.0f, -1.0f, -20.0f},
                     .max = {20.0f, 0.0f, 20.0f}};
    REQUIRE(world.CreateBody(ground).ok());
  }

  void Run(int steps) {
    for (int i = 0; i < steps; ++i) {
      world.Update(kStep);
    }
  }

  [[nodiscard]] int bodies() const { return world.Stats().bodies; }
};

// Rifle- and pistol-sized boxes.
constexpr asset::Bounds kRifleBounds{.min = {-0.03f, -0.1f, -0.8f},
                                     .max = {0.03f, 0.1f, 0.2f}};
constexpr asset::Bounds kPistolBounds{.min = {-0.015f, -0.07f, -0.1f},
                                      .max = {0.015f, 0.07f, 0.1f}};

// Definitions as the game's library would number them; the pistol's comes
// first so SetShape() grows the table out of order.
constexpr WeaponDefId kRifleDef{2};
constexpr WeaponDefId kPistolDef{0};

void SetShapes(DroppedWeapons& dropped) {
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  REQUIRE(dropped.SetShape(kPistolDef, kPistolBounds));
}

WeaponItem Pistol() {
  return {.def = kPistolDef, .firearm = Firearm(Glock17Spec())};
}

// Planned 1 m above the ground with a sideways velocity.
ThrowPlan PlanAt(glm::vec3 position) {
  ThrowPlan plan;
  plan.position = position;
  plan.linear_velocity = glm::vec3(1.0f, 0.0f, 0.0f);
  return plan;
}

// A rifle a few rounds down with its selector on auto.
WeaponItem UsedRifle() {
  Firearm rifle(M16Spec());
  (void)rifle.Update({.trigger_held = true}, kStep);
  (void)rifle.Update({}, 0.2f);
  (void)rifle.Update({.fire_mode_pressed = true}, kStep);
  return {.def = kRifleDef, .firearm = rifle};
}

TEST_CASE("a dropped weapon gets a dynamic body that falls and lands") {
  Range range;
  DroppedWeapons dropped(range.world);
  SetShapes(dropped);
  const int before = range.bodies();
  const uint32_t id =
      dropped.Add(UsedRifle(), PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  CHECK(id != 0u);
  REQUIRE(dropped.items().size() == 1);
  const DroppedWeapon& item = dropped.items().front();
  REQUIRE(item.body.has_value());
  CHECK(range.bodies() == before + 1);
  CHECK(range.world.IsDynamic(*item.body));
  CHECK(range.world.LinearVelocity(*item.body).x == doctest::Approx(1.0f));
  CHECK(dropped.FindByBody(*item.body) == &item);
  CHECK(dropped.Find(id) == &item);

  range.Run(20);
  dropped.Sync();
  CHECK(dropped.items().front().pose.position.y < 1.0f);
  range.Run(120);
  dropped.Sync();
  // Resting on the ground: its lowest point (half its 20 cm height at most
  // below the origin, or the 3 cm thickness) is near y = 0.
  const float y = dropped.items().front().pose.position.y;
  CHECK(y > 0.0f);
  CHECK(y < 0.5f);
}

TEST_CASE("picking a dropped weapon up removes its body and keeps its state") {
  Range range;
  DroppedWeapons dropped(range.world);
  SetShapes(dropped);
  const WeaponItem rifle = UsedRifle();
  const int before = range.bodies();
  const uint32_t id = dropped.Add(rifle, PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  const uint32_t other =
      dropped.Add(Pistol(), PlanAt(glm::vec3(2.0f, 1.0f, 0.0f)));
  CHECK(other != id);
  CHECK(range.bodies() == before + 2);

  const std::optional<WeaponItem> taken = dropped.Take(id);
  REQUIRE(taken.has_value());
  CHECK(range.bodies() == before + 1);
  CHECK(taken->def == kRifleDef);
  CHECK(taken->firearm.rounds_loaded() == rifle.firearm.rounds_loaded());
  CHECK(taken->firearm.fire_mode() == FireMode::kAuto);
  CHECK(dropped.Find(id) == nullptr);
  CHECK_FALSE(dropped.Take(id).has_value());
  REQUIRE(dropped.items().size() == 1);
  CHECK(dropped.items().front().id == other);
}

TEST_CASE("clearing removes every dropped weapon and body") {
  Range range;
  DroppedWeapons dropped(range.world);
  SetShapes(dropped);
  const int before = range.bodies();
  (void)dropped.Add(UsedRifle(), PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  (void)dropped.Add(Pistol(), PlanAt(glm::vec3(1.0f, 1.0f, 0.0f)));
  dropped.Clear();
  CHECK(dropped.items().empty());
  CHECK(range.bodies() == before);
}

TEST_CASE("the owner's destruction removes its bodies") {
  Range range;
  const int before = range.bodies();
  {
    DroppedWeapons dropped(range.world);
    SetShapes(dropped);
    (void)dropped.Add(UsedRifle(), PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
    CHECK(range.bodies() == before + 1);
  }
  CHECK(range.bodies() == before);
}

TEST_CASE("bodies come back where they were after the world lost them") {
  Range range;
  DroppedWeapons dropped(range.world);
  SetShapes(dropped);
  (void)dropped.Add(UsedRifle(), PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  range.Run(30);
  dropped.Sync();
  const glm::vec3 where = dropped.items().front().pose.position;

  // A world rebuild removes every body.
  range.world.RemoveAllBodies();
  range.AddGround();
  const int before = range.bodies();
  dropped.RecreateBodies();
  CHECK(range.bodies() == before + 1);
  const DroppedWeapon& item = dropped.items().front();
  REQUIRE(item.body.has_value());
  CHECK(dropped.FindByBody(*item.body) == &item);
  CHECK(glm::length(range.world.InterpolatedPose(*item.body).position - where) <
        1.0e-4f);
  CHECK(glm::length(range.world.LinearVelocity(*item.body)) == 0.0f);
}

TEST_CASE("a weapon without a body is kept, not lost") {
  Range range;
  DroppedWeapons dropped(range.world);  // No shapes set.
  const int before = range.bodies();
  const uint32_t id =
      dropped.Add(UsedRifle(), PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  CHECK(range.bodies() == before);
  REQUIRE(dropped.Find(id) != nullptr);
  CHECK_FALSE(dropped.Find(id)->body.has_value());
  dropped.Sync();
  CHECK(dropped.Find(id)->pose.position.y == 1.0f);
  const std::optional<WeaponItem> taken = dropped.Take(id);
  REQUIRE(taken.has_value());
  CHECK(taken->firearm.fire_mode() == FireMode::kAuto);
}

TEST_CASE("shapes are per definition") {
  Range range;
  DroppedWeapons dropped(range.world);
  SetShapes(dropped);
  CHECK(dropped.bounds(kRifleDef).max.z == kRifleBounds.max.z);
  CHECK(dropped.bounds(kPistolDef).max.z == kPistolBounds.max.z);
  // Unknown definitions have empty bounds and get no body.
  const WeaponDefId unknown{7};
  CHECK(dropped.bounds(unknown).size().x == 0.0f);
  const int before = range.bodies();
  const uint32_t id = dropped.Add({.def = unknown, .firearm = Firearm()},
                                  PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  CHECK(range.bodies() == before);
  CHECK_FALSE(dropped.Find(id)->body.has_value());
}

TEST_CASE("the item matrix places the prefab at the body pose") {
  DroppedWeapon item;
  item.pose.position = glm::vec3(1.0f, 2.0f, 3.0f);
  item.scale = glm::vec3(2.0f);
  const glm::mat4 matrix = DroppedWeapons::Matrix(item);
  CHECK(glm::length(glm::vec3(matrix[3]) - item.pose.position) < 1.0e-6f);
  CHECK(glm::length(glm::vec3(matrix[0])) == doctest::Approx(2.0f));
}

TEST_CASE("a dropped weapon keeps its parts and handling") {
  Range range;
  DroppedWeapons dropped(range.world);
  SetShapes(dropped);
  const parts::PartCatalog catalog = parts::testing::GlockCatalog();
  const parts::PresetDef* preset = catalog.FindPreset("glock17");
  REQUIRE(preset != nullptr);
  const auto build = parts::MakeBuild(catalog, *preset);
  REQUIRE(build.ok());
  WeaponItem pistol = Pistol();
  pistol.build = build.value();
  pistol.switch_time_scale = 1.4f;
  pistol.scope_zero = 50.0f;
  const uint32_t id = dropped.Add(pistol, PlanAt(glm::vec3(0.0f, 1.0f, 0.0f)));
  range.Run(10);
  dropped.Sync();
  std::optional<WeaponItem> taken = dropped.Take(id);
  REQUIRE(taken.has_value());
  CHECK(taken->build == build.value());
  CHECK(taken->switch_time_scale == 1.4f);
  CHECK(taken->scope_zero == 50.0f);
}

}  // namespace
}  // namespace blue::weapon
