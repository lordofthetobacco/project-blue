#include "game/scene_pickups.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "interact/interaction_focus.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "render/scene_view.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "tests/scratch_directory.h"
#include "tests/world_fixtures.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"
#include "world/prefab_cache.h"
#include "world/world.h"
#include "world/world_physics.h"

namespace blue::game {
namespace {

// A unit cube tagged as a glock17, with the weapon sockets, a dynamic box
// body and a point light.
prefab::Prefab MakeWeaponCube() {
  prefab::Prefab prefab = testing::MakeCubePrefab({"steel"}, true);
  prefab.physics.dynamic = true;
  prefab.physics.mass = 0.9f;
  for (const auto& [name, z] :
       {std::pair{"muzzle", 0.5f}, std::pair{"sight", -0.4f},
        std::pair{"grip", -0.3f}}) {
    prefab::Socket socket;
    socket.name = name;
    socket.local.translation = glm::vec3(0.0f, 0.4f, z);
    prefab.sockets.push_back(socket);
  }
  prefab.lights.push_back({.name = "lamp", .node = 0, .intensity = 3.0f});
  prefab::SetTag(prefab, weapon::kWeaponTag, "glock17");
  return prefab;
}

scene::InstanceId Place(scene::SceneData& scene, const std::string& path,
                        glm::vec3 position,
                        scene::InstanceId parent = scene::kNoInstance) {
  scene::Instance instance;
  instance.prefab = scene::AddPrefab(scene, path);
  instance.transform.position = position;
  instance.parent = parent;
  return scene::AddInstance(scene, std::move(instance));
}

// A scene with a floor, a weapon at the root, a prop and a weapon nested
// under the prop, built into a world with bodies.
struct Range {
  testing::ScratchDirectory dir{"scene_pickups"};
  scene::SceneData scene;
  scene::InstanceId floor = scene::kNoInstance;
  scene::InstanceId weapon = scene::kNoInstance;
  scene::InstanceId prop = scene::kNoInstance;
  scene::InstanceId nested = scene::kNoInstance;
  testing::FakeGpu gpu;
  world::PrefabCache cache{gpu};
  world::World world{cache};
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld physics{runtime};
  world::WorldPhysics bodies{physics};
  weapon::WeaponLibrary library;
  ScenePickups pickups;

  Range() {
    const std::string weapon_path = dir.Child("weapon.pbo");
    const std::string box_path = dir.Child("box.pbo");
    REQUIRE(prefab::SavePrefab(MakeWeaponCube(), weapon_path).ok());
    REQUIRE(prefab::SavePrefab(testing::MakeCubePrefab({"a"}, true), box_path)
                .ok());
    floor = Place(scene, box_path, {0.0f, -0.5f, 0.0f});
    scene::FindInstance(scene, floor)->transform.scale = {20.0f, 1.0f, 20.0f};
    weapon = Place(scene, weapon_path, {2.0f, 0.5f, 0.0f});
    prop = Place(scene, box_path, {-3.0f, 0.5f, 0.0f});
    nested = Place(scene, weapon_path, {0.0f, 1.0f, 0.0f}, prop);
    Build();
  }

  void Build() {
    REQUIRE(world.Build(scene).empty());
    (void)bodies.Build(scene, world);
    pickups.Find(scene, world, [this](world::PrefabId id) {
      return library.Register(static_cast<uint32_t>(id), cache.prefab(id),
                              cache.key(id));
    });
  }

  [[nodiscard]] size_t DrawCount() const {
    std::vector<render::DrawItem> draws;
    world.AppendDraws(draws);
    return draws.size();
  }

  [[nodiscard]] float LightIntensity() const {
    float total = 0.0f;
    for (const render::PointLight& light : world.point_lights()) {
      total += light.intensity;
    }
    return total;
  }
};

TEST_CASE("A root instance of a weapon prefab is a pick-up candidate") {
  Range range;
  REQUIRE(range.pickups.pickups().size() == 1);  // Not the nested one.
  const ScenePickup& pickup = range.pickups.pickups().front();
  CHECK(pickup.instance == range.weapon);
  CHECK(std::string(range.library.def(pickup.def).type.id) == "glock17");
  CHECK_FALSE(pickup.taken);

  std::vector<interact::Interactable> candidates;
  range.pickups.AppendCandidates(range.world, candidates);
  REQUIRE(candidates.size() == 1);
  CHECK(candidates[0].id == range.weapon);
  CHECK(candidates[0].kind == interact::InteractableKind::kSceneWeapon);
  CHECK(glm::length(candidates[0].shape.center - glm::vec3(2.0f, 0.5f, 0.0f)) <
        1.0e-4f);
  CHECK(candidates[0].shape.half_extents.x == doctest::Approx(0.5f));
}

TEST_CASE("Picking a scene weapon up removes its draws, body and lights") {
  Range range;
  const size_t draws = range.DrawCount();
  const float lights = range.LightIntensity();
  REQUIRE(lights > 0.0f);
  REQUIRE(range.bodies.BodyFor(range.weapon).has_value());
  const int body_count = range.physics.Stats().bodies;

  std::optional<weapon::WeaponItem> taken = range.pickups.Take(
      range.weapon, range.world, range.bodies, range.library);
  REQUIRE(taken.has_value());
  CHECK(taken->def == range.pickups.pickups().front().def);
  CHECK(taken->firearm.rounds_loaded() == 18);  // Fresh: 17 + 1.
  CHECK(taken->firearm.spare_magazines().size() == 3);

  CHECK(range.world.IsInstanceHidden(range.weapon));
  CHECK_FALSE(range.world.IsInstanceHidden(range.prop));
  CHECK(range.DrawCount() == draws - 1);  // One cube mesh.
  // The nested weapon's light stays; the taken one's is gone.
  CHECK(range.LightIntensity() == doctest::Approx(lights - 3.0f));
  CHECK(range.physics.Stats().bodies == body_count - 1);
  CHECK_FALSE(range.bodies.BodyFor(range.weapon).has_value());
  CHECK_FALSE(range.world.InstanceBounds(range.weapon).has_value());
  // Nothing is hit where it was.
  const std::optional<physics::RayHit> hit =
      range.physics.CastRay({2.0f, 3.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, 2.0f);
  CHECK_FALSE(hit.has_value());

  std::vector<interact::Interactable> candidates;
  range.pickups.AppendCandidates(range.world, candidates);
  CHECK(candidates.empty());
  CHECK_FALSE(
      range.pickups.Take(range.weapon, range.world, range.bodies, range.library)
          .has_value());

  // A rebuild (F9) brings it back, from the unchanged scene data.
  range.Build();
  CHECK(range.DrawCount() == draws);
  CHECK(range.LightIntensity() == doctest::Approx(lights));
  CHECK(range.bodies.BodyFor(range.weapon).has_value());
  REQUIRE(range.pickups.pickups().size() == 1);
  CHECK_FALSE(range.pickups.pickups().front().taken);
  CHECK(range.library.size() == 1);  // The same definition.
}

TEST_CASE("Hiding an instance hides its descendants and keeps selections") {
  Range range;
  const size_t draws = range.DrawCount();
  REQUIRE(range.world.HideInstance(range.prop));
  CHECK(range.world.IsInstanceHidden(range.nested));
  CHECK(range.DrawCount() == draws - 2);
  CHECK(range.bodies.RemoveHiddenBodies(range.world) == 2);
  CHECK(range.bodies.RemoveHiddenBodies(range.world) == 0);
  CHECK(range.world.HideInstance(range.prop));  // Again: no-op.
  CHECK_FALSE(range.world.HideInstance(9999));

  // Selection indices still match the draws actually appended.
  std::vector<render::DrawItem> appended;
  range.world.AppendDraws(appended);
  std::vector<uint32_t> selected;
  const scene::InstanceId selection[] = {range.weapon};
  range.world.AppendSelectedDraws(selection, 0, selected);
  REQUIRE(selected.size() == 1);
  REQUIRE(selected[0] < appended.size());
  CHECK(glm::length(glm::vec3(appended[selected[0]].transform[3]) -
                    glm::vec3(2.0f, 0.5f, 0.0f)) < 1.0e-4f);
}

}  // namespace
}  // namespace blue::game
