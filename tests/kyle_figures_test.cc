// The "Robot Kyle" figures blue-pack samples cooks into the build's assets
// (target blue_sample_assets runs before the tests): posed figures holding a
// nested weapon that is no pick-up, standing in the range scene.

#include <doctest/doctest.h>

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/transform.h"
#include "cook/pose_file.h"
#include "core/path.h"
#include "game/scene_pickups.h"
#include "physics/physics_runtime.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "sim/sim_world.h"
#include "tests/world_fixtures.h"
#include "weapon/weapon_def.h"
#include "world/prefab_cache.h"
#include "world/world.h"

namespace blue {
namespace {

struct Figure {
  const char* name;
  const char* instance;
  const char* weapon;
};
constexpr Figure kFigures[] = {{"kyle_rifle", "kyle rifle", "m16.pbo"},
                               {"kyle_pistol", "kyle pistol", "glock17.pbo"}};

std::string Asset(std::string_view name) {
  return core::JoinPath(BLUE_ASSETS_DIR, name);
}

bool Near(glm::vec3 a, glm::vec3 b, float tolerance = 1.0e-5f) {
  return glm::length(a - b) <= tolerance;
}

const scene::Instance* FindByName(const scene::SceneData& scene,
                                  std::string_view name) {
  for (const scene::Instance& instance : scene.instances) {
    if (instance.name == name) {
      return &instance;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("The Kyle figures hold their weapon as a nested prefab") {
  physics::PhysicsRuntime runtime;
  for (const Figure& figure : kFigures) {
    CAPTURE(figure.name);
    auto loaded = prefab::LoadPrefab(Asset(std::string(figure.name) + ".pbo"));
    REQUIRE(loaded.ok());
    const prefab::Prefab& prefab = loaded.value();
    CHECK(prefab::ValidatePrefab(prefab) == std::nullopt);

    // A 1.8 m figure standing on the origin.
    CHECK(prefab.meshes.size() == 1);
    CHECK(prefab.bounds.min.y == doctest::Approx(0.0f).epsilon(0.005));
    CHECK(prefab.bounds.max.y == doctest::Approx(1.8f).epsilon(0.03));
    REQUIRE(prefab.materials.size() == 1);
    CHECK(prefab.materials[0].name == "kyle");
    CHECK(!prefab::FindTag(prefab, weapon::kWeaponTag));

    // The weapon, at the transform the bake wrote.
    auto pose = cook::LoadPoseFile(core::JoinPath(
        BLUE_SAMPLES_DIR, std::string("kyle/") + figure.name + ".pose"));
    REQUIRE(pose.ok());
    CHECK(pose.value().weapon == figure.weapon);
    REQUIRE(prefab.children.size() == 1);
    const prefab::ChildPrefab& child = prefab.children[0];
    CHECK(child.node == -1);
    CHECK(core::NormalizePath(child.path) ==
          core::NormalizePath(Asset(figure.weapon)));
    CHECK(Near(child.local.translation, pose.value().weapon_local.translation));
    CHECK(glm::dot(child.local.rotation, pose.value().weapon_local.rotation) ==
          doctest::Approx(1.0f));
    // In front of the figure, between hip and head.
    CHECK(child.local.translation.y > 0.9f);
    CHECK(child.local.translation.y < 1.7f);

    for (const char* hand : {"hand_r", "hand_l"}) {
      CAPTURE(hand);
      const int socket = prefab::FindSocket(prefab, hand);
      REQUIRE(socket >= 0);
      CHECK(prefab.sockets[static_cast<size_t>(socket)].node == -1);
    }
    // The firing hand's palm is on the weapon's grip.
    auto weapon = prefab::LoadPrefab(Asset(figure.weapon));
    REQUIRE(weapon.ok());
    const int grip = prefab::FindSocket(weapon.value(), "grip");
    REQUIRE(grip >= 0);
    const glm::vec3 grip_in_figure = glm::vec3(
        animation::ToMatrix(child.local) *
        glm::vec4(
            weapon.value().sockets[static_cast<size_t>(grip)].local.translation,
            1.0f));
    const prefab::Socket& hand_r =
        prefab
            .sockets[static_cast<size_t>(prefab::FindSocket(prefab, "hand_r"))];
    CHECK(glm::length(hand_r.local.translation - grip_in_figure) < 0.06f);

    // One static capsule round the body.
    CHECK(prefab.physics.kind == prefab::ColliderKind::kCapsule);
    CHECK_FALSE(prefab.physics.dynamic);
    CHECK(prefab.physics.center.y == doctest::Approx(0.9f));
    CHECK(prefab.physics.radius + prefab.physics.half_height ==
          doctest::Approx(0.9f));
    CHECK(physics::CookedShape::Restore(prefab.physics.cooked_shape).ok());
  }
}

TEST_CASE("The range figures stand beside the player start, no pick-ups") {
  auto loaded = scene::LoadMscn(Asset("range.mscn"));
  REQUIRE(loaded.ok());
  const scene::SceneData& scene = loaded.value();
  std::array<scene::InstanceId, std::size(kFigures)> ids{};
  for (size_t i = 0; i < std::size(kFigures); ++i) {
    CAPTURE(kFigures[i].instance);
    const scene::Instance* instance = FindByName(scene, kFigures[i].instance);
    REQUIRE(instance != nullptr);
    ids[i] = instance->id;
    CHECK(instance->parent == scene::kNoInstance);
    CHECK(Near(
        instance->transform.position,
        {scene.player_start.position.x + 3.0f + 1.5f * static_cast<float>(i),
         0.0f, scene.player_start.position.z}));
  }

  testing::FakeGpu gpu;
  world::PrefabCache cache(gpu);
  world::World world(cache);
  REQUIRE(world.Build(scene).empty());
  // Each figure places its own prefab and the nested weapon under it.
  for (const scene::InstanceId id : ids) {
    int nested_weapons = 0;
    for (const world::Placement& placement : world.placements()) {
      if (placement.owner == id && placement.parent != -1 &&
          prefab::FindTag(cache.prefab(placement.prefab), weapon::kWeaponTag)) {
        ++nested_weapons;
      }
    }
    CHECK(nested_weapons == 1);
  }

  // The game's pick-up rule skips them (every weapon prefab counts).
  weapon::WeaponLibrary library;
  game::ScenePickups pickups;
  pickups.Find(scene, world, [&](world::PrefabId id) {
    return library.Register(static_cast<uint32_t>(id), cache.prefab(id),
                            cache.key(id));
  });
  for (const game::ScenePickup& pickup : pickups.pickups()) {
    for (const scene::InstanceId id : ids) {
      CHECK(pickup.instance != id);
    }
  }
}

TEST_CASE("The server simulation keeps the figures solid and not pick-ups") {
  physics::PhysicsRuntime runtime;
  auto created = sim::SimWorld::Create(
      runtime, {.asset_dir = BLUE_ASSETS_DIR, .scene_path = "range.mscn"});
  REQUIRE(created.ok());
  const sim::SimWorld& world = *created.value();

  std::vector<sim::PickupReadout> pickups(world.ReadPickups({}));
  world.ReadPickups(pickups);
  for (const Figure& figure : kFigures) {
    CAPTURE(figure.instance);
    const scene::Instance* instance =
        FindByName(world.scene(), figure.instance);
    REQUIRE(instance != nullptr);
    for (const sim::PickupReadout& pickup : pickups) {
      CHECK(pickup.instance != instance->id);
    }
    // A shot from beside it at chest height stops on its body.
    const glm::vec3 chest =
        instance->transform.position + glm::vec3(0, 1.3f, 0);
    const std::optional<sim::SimRayHit> hit = world.CastRay(
        chest + glm::vec3(0.0f, 0.0f, 3.0f), {0.0f, 0.0f, -1.0f}, 10.0f);
    REQUIRE(hit.has_value());
    CHECK(hit->instance == instance->id);
  }
  MESSAGE("range.mscn content hash: ", world.content_hash());
}

}  // namespace blue
