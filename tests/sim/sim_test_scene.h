#ifndef BLUE_TESTS_SIM_SIM_TEST_SCENE_H_
#define BLUE_TESTS_SIM_SIM_TEST_SCENE_H_

#include <doctest/doctest.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "camera/look.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/mscn_serializer.h"
#include "scene/scene.h"
#include "scene/scene_data.h"
#include "sim/sim_world.h"
#include "sim/user_command.h"
#include "tests/scratch_directory.h"

// A small scene written to a scratch directory for the headless simulation
// tests (SimWorld, the C ABI, the determinism gate): a 40 m floor whose top
// is y = 0, a 1 m dynamic crate, a 4 m static wall, and two weapon prefabs
// (a "glock17" pistol and an "m16" rifle, the starting loadout) of which
// the rifle also lies in the scene as a pick-up. The player start is at
// (0, 1.65, 0) looking down -Z.

namespace blue::sim::testing {

inline constexpr scene::InstanceId kFloorInstance = 1;
inline constexpr scene::InstanceId kCrateInstance = 2;
inline constexpr scene::InstanceId kWallInstance = 3;
inline constexpr scene::InstanceId kPickupInstance = 4;
inline constexpr glm::vec3 kCratePosition{0.0f, 0.5f, -8.0f};
inline constexpr glm::vec3 kWallPosition{0.0f, 2.0f, 12.0f};
inline constexpr glm::vec3 kPickupPosition{-6.0f, 0.1f, 0.0f};

// A prefab of one node with a box collider of `half_extents`.
inline prefab::Prefab MakeBoxPrefab(glm::vec3 half_extents, bool dynamic,
                                    float mass) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  prefab.physics.kind = prefab::ColliderKind::kBox;
  prefab.physics.half_extents = half_extents;
  prefab.physics.dynamic = dynamic;
  prefab.physics.mass = mass;
  prefab.bounds = {.min = -half_extents, .max = half_extents};
  return prefab;
}

inline prefab::Socket MakeSocket(std::string name, glm::vec3 position) {
  prefab::Socket socket;
  socket.name = std::move(name);
  socket.local.translation = position;
  return socket;
}

// A single-piece weapon model (a small cube) tagged `weapon = type`, with
// muzzle, sight and grip sockets.
inline prefab::Prefab MakeWeaponPrefab(std::string type) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "root", .parent = -1, .rest = {}}};
  asset::MeshData cube = asset::MakeCube();
  for (asset::Vertex& vertex : cube.vertices) {
    vertex.position *= glm::vec3(0.05f, 0.1f, 0.4f);
  }
  const asset::Bounds bounds{
      .min = cube.bounds.min * glm::vec3(0.05f, 0.1f, 0.4f),
      .max = cube.bounds.max * glm::vec3(0.05f, 0.1f, 0.4f)};
  prefab::Mesh mesh{.name = "cube", .node = 0, .bounds = bounds};
  mesh.vertices =
      prefab.storage.StoreArray<asset::Vertex>(std::span(cube.vertices));
  mesh.indices =
      prefab.storage.StoreArray<asset::Index>(std::span(cube.indices));
  mesh.submeshes.push_back(
      {.first_index = 0,
       .index_count = static_cast<uint32_t>(cube.indices.size()),
       .material = 0});
  prefab.materials.push_back({.name = "steel"});
  prefab.meshes.push_back(std::move(mesh));
  prefab.bounds = bounds;
  prefab.sockets = {MakeSocket("muzzle", {0.0f, 0.0f, 0.2f}),
                    MakeSocket("sight", {0.0f, 0.05f, -0.1f}),
                    MakeSocket("grip", {0.0f, -0.08f, -0.05f})};
  prefab.tags.push_back({.key = "weapon", .value = std::move(type)});
  return prefab;
}

inline scene::Instance MakeInstance(scene::InstanceId id, int prefab,
                                    glm::vec3 position) {
  scene::Instance instance;
  instance.id = id;
  instance.name = "instance " + std::to_string(id);
  instance.prefab = prefab;
  instance.transform.position = position;
  return instance;
}

// The scene's files in a scratch directory, and SimWorldOptions for it.
struct TestScene {
  blue::testing::ScratchDirectory dir{"sim_scene"};

  TestScene() {
    Save(MakeBoxPrefab(glm::vec3(20.0f, 0.5f, 20.0f), false, 1.0f),
         "floor.pbo");
    Save(MakeBoxPrefab(glm::vec3(0.5f), true, 4.0f), "crate.pbo");
    Save(MakeBoxPrefab(glm::vec3(2.0f, 2.0f, 0.25f), false, 1.0f), "wall.pbo");
    Save(MakeWeaponPrefab("glock17"), "weapons/pistol.pbo");
    Save(MakeWeaponPrefab("m16"), "weapons/rifle.pbo");

    scene::SceneData scene;
    scene.prefabs = {dir.Child("floor.pbo"), dir.Child("crate.pbo"),
                     dir.Child("wall.pbo"), dir.Child("weapons/rifle.pbo")};
    scene.instances = {
        MakeInstance(kFloorInstance, 0, glm::vec3(0.0f, -0.5f, 0.0f)),
        MakeInstance(kCrateInstance, 1, kCratePosition),
        MakeInstance(kWallInstance, 2, kWallPosition),
        MakeInstance(kPickupInstance, 3, kPickupPosition)};
    scene.next_instance_id = 5;
    scene.player_start = {.position = glm::vec3(0.0f, 1.65f, 0.0f),
                          .yaw_radians = 0.0f,
                          .pitch_radians = 0.0f};
    REQUIRE(scene::SaveMscn(scene, dir.Child("test.mscn")).ok());
  }

  void Save(const prefab::Prefab& prefab, std::string_view path) const {
    REQUIRE(prefab::SavePrefab(prefab, dir.Child(path)).ok());
  }

  [[nodiscard]] SimWorldOptions Options() const {
    return {.asset_dir = dir.path(),
            .scene_path = "test.mscn",
            .worker_threads = 0,
            .max_players = 8,
            .starting_weapons = {{.path = "weapons/pistol.pbo"},
                                 {.path = "weapons/rifle.pbo"}}};
  }
};

// Returns a command for `tick` that looks from `shooter`'s eye at `target`.
inline UserCommand AimAt(const SimWorld& world, uint32_t shooter,
                         glm::vec3 target, uint32_t tick, uint32_t held = 0,
                         uint32_t pressed = 0) {
  const PlayerSim* player = world.player(shooter);
  REQUIRE(player != nullptr);
  const camera::LookAngles look = camera::LookTowards(player->eye(), target);
  UserCommand command;
  command.tick = tick;
  command.held = held;
  command.pressed = pressed;
  command.yaw = look.yaw;
  command.pitch = look.pitch;
  return command;
}

// Returns the point `height_fraction` up player `id`'s current capsule.
inline glm::vec3 CapsulePoint(const SimWorld& world, uint32_t id,
                              float height_fraction) {
  const std::optional<Capsule> capsule = world.CurrentCapsule(id);
  REQUIRE(capsule.has_value());
  return capsule->feet +
         glm::vec3(0.0f, capsule->height * height_fraction, 0.0f);
}

// Runs one full server tick: Step, the sweep, hits against the current
// capsules and the resolve.
inline void RunTick(SimWorld& world, uint32_t tick) {
  world.Step(tick);
  (void)world.SweepProjectiles();
  std::vector<ShotHit> hits;
  world.HitCapsulesNow(hits);
  world.ResolveProjectiles(hits);
}

// Returns every queued event.
inline std::vector<SimEvent> DrainEvents(SimWorld& world) {
  std::vector<SimEvent> events(world.pending_events());
  CHECK(world.PollEvents(events) == events.size());
  return events;
}

}  // namespace blue::sim::testing

#endif  // BLUE_TESTS_SIM_SIM_TEST_SCENE_H_
