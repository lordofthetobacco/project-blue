#include <doctest/doctest.h>

#include <cstdint>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "weapon/dropped_weapons.h"
#include "weapon/firearm.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_specs.h"
#include "weapon/weapon_throw.h"

namespace blue::weapon {
namespace {

constexpr float kStep = 1.0f / 60.0f;
constexpr WeaponDefId kRifleDef{1};
constexpr asset::Bounds kRifleBounds{.min = {-0.03f, -0.1f, -0.8f},
                                     .max = {0.03f, 0.1f, 0.2f}};

// A world with gravity and a static ground whose surface is y = 0.
struct Range {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime};

  Range() {
    world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
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

WeaponItem Rifle() { return {.def = kRifleDef, .firearm = Firearm(M16Spec())}; }

physics::Pose PoseAt(glm::vec3 position) {
  return {.position = position,
          .rotation = glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f))};
}

bool SameMatrix(const glm::mat4& a, const glm::mat4& b) {
  for (int c = 0; c < 4; ++c) {
    if (glm::length(a[c] - b[c]) > 1.0e-5f) {
      return false;
    }
  }
  return true;
}

TEST_CASE("a replicated weapon keeps its id and stays where it is put") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  const int before = range.bodies();
  const physics::Pose pose = PoseAt(glm::vec3(1.0f, 2.0f, 3.0f));
  REQUIRE(dropped.AddReplicated(42, Rifle(), pose));
  CHECK(range.bodies() == before + 1);
  const DroppedWeapon* item = dropped.Find(42);
  REQUIRE(item != nullptr);
  CHECK(item->id == 42u);
  CHECK(item->replicated);
  REQUIRE(item->body.has_value());
  CHECK(dropped.FindByBody(*item->body) == item);
  CHECK(item->weapon.def == kRifleDef);

  // Gravity does not move it: only SetPose() does.
  range.Run(30);
  dropped.Sync();
  CHECK(glm::length(dropped.Find(42)->pose.position - pose.position) == 0.0f);
  CHECK(glm::length(range.world.InterpolatedPose(*item->body).position -
                    pose.position) < 1.0e-4f);
  CHECK(SameMatrix(
      DroppedWeapons::Matrix(*item),
      PlanMatrix({.position = pose.position, .rotation = pose.rotation})));
}

TEST_CASE("ids in use and id 0 are refused") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  REQUIRE(
      dropped.AddReplicated(7, Rifle(), PoseAt(glm::vec3(0.0f, 1.0f, 0.0f))));
  const int bodies = range.bodies();
  CHECK_FALSE(dropped.AddReplicated(7, Rifle(), PoseAt(glm::vec3(1.0f))));
  CHECK_FALSE(dropped.AddReplicated(0, Rifle(), PoseAt(glm::vec3(1.0f))));
  CHECK(dropped.items().size() == 1);
  CHECK(range.bodies() == bodies);
}

TEST_CASE("SetPose moves a replicated weapon and its body") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  REQUIRE(
      dropped.AddReplicated(5, Rifle(), PoseAt(glm::vec3(0.0f, 1.0f, 0.0f))));
  const physics::Pose moved = PoseAt(glm::vec3(4.0f, 0.5f, -2.0f));
  CHECK(dropped.SetPose(5, moved));
  CHECK_FALSE(dropped.SetPose(6, moved));
  const DroppedWeapon* item = dropped.Find(5);
  REQUIRE(item != nullptr);
  CHECK(item->pose.position == moved.position);
  range.Run(2);
  dropped.Sync();
  REQUIRE(item->body.has_value());
  CHECK(glm::length(range.world.InterpolatedPose(*item->body).position -
                    moved.position) < 1.0e-4f);
  CHECK(dropped.Find(5)->pose.position == moved.position);
}

TEST_CASE("Remove and Take drop a replicated weapon and its body") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  const int before = range.bodies();
  REQUIRE(
      dropped.AddReplicated(3, Rifle(), PoseAt(glm::vec3(0.0f, 1.0f, 0.0f))));
  REQUIRE(
      dropped.AddReplicated(4, Rifle(), PoseAt(glm::vec3(1.0f, 1.0f, 0.0f))));
  CHECK(dropped.Remove(3));
  CHECK_FALSE(dropped.Remove(3));
  CHECK(dropped.Find(3) == nullptr);
  CHECK_FALSE(dropped.Contains(3));
  CHECK(dropped.Contains(4));
  CHECK(range.bodies() == before + 1);
  const std::optional<WeaponItem> taken = dropped.Take(4);
  REQUIRE(taken.has_value());
  CHECK(taken->def == kRifleDef);
  CHECK(range.bodies() == before);
  // A removed id can be replicated again.
  CHECK(dropped.AddReplicated(3, Rifle(), PoseAt(glm::vec3(0.0f, 1.0f, 0.0f))));
}

TEST_CASE("local drops skip ids taken by replicated ones") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  REQUIRE(
      dropped.AddReplicated(1, Rifle(), PoseAt(glm::vec3(0.0f, 1.0f, 0.0f))));
  REQUIRE(
      dropped.AddReplicated(2, Rifle(), PoseAt(glm::vec3(1.0f, 1.0f, 0.0f))));
  ThrowPlan plan;
  plan.position = glm::vec3(2.0f, 1.0f, 0.0f);
  const uint32_t local = dropped.Add(Rifle(), plan);
  CHECK(local == 3u);
  CHECK_FALSE(dropped.Find(local)->replicated);
  CHECK_FALSE(dropped.AddReplicated(3, Rifle(), PoseAt(glm::vec3(0.0f))));
}

TEST_CASE("local drops still fall and draw at their plan") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  ThrowPlan plan;
  plan.position = glm::vec3(0.0f, 1.0f, 0.0f);
  plan.rotation = glm::angleAxis(0.3f, glm::vec3(1.0f, 0.0f, 0.0f));
  plan.scale = glm::vec3(1.5f);
  const uint32_t id = dropped.Add(Rifle(), plan);
  CHECK(id == 1u);
  const DroppedWeapon* item = dropped.Find(id);
  REQUIRE(item != nullptr);
  CHECK(SameMatrix(DroppedWeapons::Matrix(*item), PlanMatrix(plan)));
  REQUIRE(item->body.has_value());
  CHECK(range.world.IsDynamic(*item->body));
  range.Run(20);
  dropped.Sync();
  CHECK(dropped.Find(id)->pose.position.y < 1.0f);
}

TEST_CASE("replicated bodies come back kinematic after the world lost them") {
  Range range;
  DroppedWeapons dropped(range.world);
  REQUIRE(dropped.SetShape(kRifleDef, kRifleBounds));
  const physics::Pose pose = PoseAt(glm::vec3(0.0f, 3.0f, 0.0f));
  REQUIRE(dropped.AddReplicated(9, Rifle(), pose));
  range.world.RemoveAllBodies();
  dropped.RecreateBodies();
  range.Run(30);
  dropped.Sync();
  const DroppedWeapon* item = dropped.Find(9);
  REQUIRE(item != nullptr);
  REQUIRE(item->body.has_value());
  CHECK(glm::length(range.world.InterpolatedPose(*item->body).position -
                    pose.position) < 1.0e-4f);
}

}  // namespace
}  // namespace blue::weapon
