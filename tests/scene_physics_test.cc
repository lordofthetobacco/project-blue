#include "physics/scene_physics.h"

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <string_view>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

namespace blue::physics {
namespace {

constexpr float kFrame = 1.0f / 60.0f;
constexpr size_t kGround = 0;
constexpr size_t kCube = 1;

// Owns the CPU meshes behind the builtin names the tests use.
class TestMeshes {
 public:
  TestMeshes()
      : cube_(asset::MakeCube(glm::vec3(1.0f))),
        plane_(asset::MakePlane(12.0f, glm::vec3(1.0f))) {}

  [[nodiscard]] MeshSource Source() const {
    return [this](std::string_view path) -> std::optional<MeshView> {
      if (path == "builtin:cube") {
        return MeshView{.bounds = cube_.bounds, .mesh = &cube_};
      }
      if (path == "builtin:plane") {
        return MeshView{.bounds = plane_.bounds, .mesh = &plane_};
      }
      return std::nullopt;
    };
  }

 private:
  asset::MeshData cube_;
  asset::MeshData plane_;
};

// A static ground plane (entity 0) and a dynamic cube at y = 3 (entity 1).
scene::Scene MakeDefaultScene() {
  scene::Scene scene;
  scene.entities.push_back({.name = "ground", .mesh_path = "builtin:plane"});
  scene.entities[kGround].body =
      scene::RigidBody{.motion = scene::MotionType::kStatic};

  scene::Transform cube;
  cube.position.y = 3.0f;
  cube.scale = {1.5f, 1.0f, 1.0f};
  scene.entities.push_back(
      {.name = "cube", .mesh_path = "builtin:cube", .transform = cube});
  scene.entities[kCube].body = scene::RigidBody{};
  return scene;
}

void Simulate(PhysicsWorld& world, ScenePhysics& physics, scene::Scene& scene,
              float seconds) {
  for (float t = 0.0f; t < seconds; t += kFrame) {
    world.Update(kFrame);
    physics.SyncToScene(scene);
  }
}

TEST_CASE("a dropped cube comes to rest on the ground plane") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  scene::Scene scene = MakeDefaultScene();
  const scene::Transform ground_before = scene.entities[kGround].transform;

  const RebuildSummary summary = physics.Rebuild(scene, meshes.Source());
  CHECK(summary.created == 2);
  CHECK(summary.failed == 0);

  Simulate(world, physics, scene, 3.0f);

  const scene::Transform& cube = scene.entities[kCube].transform;
  CHECK(cube.position.y == doctest::Approx(0.5f).epsilon(0.04));
  CHECK(std::abs(cube.position.x) < 0.05f);
  const scene::Transform& ground = scene.entities[kGround].transform;
  CHECK(ground.position == ground_before.position);
  CHECK(ground.rotation == ground_before.rotation);
  CHECK(ground.scale == ground_before.scale);
}

TEST_CASE("SyncToScene keeps the entity scale") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  scene::Scene scene = MakeDefaultScene();
  physics.Rebuild(scene, meshes.Source());

  Simulate(world, physics, scene, 0.5f);

  CHECK(scene.entities[kCube].transform.scale == glm::vec3(1.5f, 1.0f, 1.0f));
  CHECK(scene.entities[kCube].transform.position.y < 3.0f);
}

TEST_CASE("an entity whose mesh is unavailable is skipped and counted") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  scene::Scene scene = MakeDefaultScene();
  scene.entities[kCube].mesh_path = "missing.obj";

  const RebuildSummary summary = physics.Rebuild(scene, meshes.Source());

  CHECK(summary.created == 1);
  CHECK(summary.failed == 1);
  CHECK(physics.BodyFor(kGround).has_value());
  CHECK_FALSE(physics.BodyFor(kCube).has_value());
  CHECK(world.Stats().bodies == 1);
}

TEST_CASE("entities without a body get none") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  scene::Scene scene = MakeDefaultScene();
  scene.entities[kCube].body.reset();

  const RebuildSummary summary = physics.Rebuild(scene, meshes.Source());

  CHECK(summary.created == 1);
  CHECK(summary.failed == 0);
  CHECK_FALSE(physics.BodyFor(kCube).has_value());
  CHECK_FALSE(physics.BodyFor(99).has_value());
}

TEST_CASE("Rebuild twice does not duplicate bodies") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  const scene::Scene scene = MakeDefaultScene();

  physics.Rebuild(scene, meshes.Source());
  physics.Rebuild(scene, meshes.Source());

  CHECK(world.Stats().bodies == 2);
}

TEST_CASE("entities and bodies map to each other") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  const scene::Scene scene = MakeDefaultScene();
  physics.Rebuild(scene, meshes.Source());

  const std::optional<BodyId> body = physics.BodyFor(kCube);
  REQUIRE(body.has_value());
  CHECK(physics.EntityFor(*body) == std::optional<size_t>(kCube));
  CHECK(world.IsDynamic(*body));
  CHECK_FALSE(physics.EntityFor(static_cast<BodyId>(12345)).has_value());
}

TEST_CASE("the scene's gravity drives the simulation") {
  const TestMeshes meshes;
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  ScenePhysics physics(world);
  scene::Scene scene = MakeDefaultScene();

  SUBCASE("zero gravity keeps the cube in place") {
    scene.gravity = glm::vec3(0.0f);
    physics.Rebuild(scene, meshes.Source());
    Simulate(world, physics, scene, 1.0f);
    CHECK(scene.entities[kCube].transform.position.y ==
          doctest::Approx(3.0f).epsilon(0.001));
  }

  SUBCASE("sideways gravity moves it sideways") {
    scene.gravity = glm::vec3(5.0f, 0.0f, 0.0f);
    physics.Rebuild(scene, meshes.Source());
    Simulate(world, physics, scene, 1.0f);
    CHECK(scene.entities[kCube].transform.position.x > 1.0f);
    CHECK(scene.entities[kCube].transform.position.y ==
          doctest::Approx(3.0f).epsilon(0.001));
  }
}

}  // namespace
}  // namespace blue::physics
