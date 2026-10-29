#include "physics/physics_world.h"

#include <doctest/doctest.h>

#include <cmath>
#include <optional>
#include <string>

#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "asset/model_importer.h"
#include "physics/physics_runtime.h"
#include "scene/scene.h"

namespace blue::physics {
namespace {

constexpr float kStep = 1.0f / 60.0f;
const glm::vec3 kNoGravity(0.0f);

// Returns a description of a dynamic unit-radius-ish body: bounds of a 1 m
// cube centered at the origin.
BodyDesc CubeDesc(scene::ShapeType shape, scene::MotionType motion,
                  glm::vec3 position) {
  BodyDesc desc;
  desc.body.shape = shape;
  desc.body.motion = motion;
  desc.body.mass = 10.0f;
  desc.pose.position = position;
  desc.bounds = asset::Bounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
  desc.shape_key = "test:cube";
  return desc;
}

// A flat 100 x 100 m ground plane at y = 0, as MakePlane produces.
BodyDesc GroundDesc() {
  BodyDesc desc;
  desc.body.shape = scene::ShapeType::kBox;
  desc.body.motion = scene::MotionType::kStatic;
  desc.bounds = asset::Bounds{glm::vec3(-50.0f, 0.0f, -50.0f),
                              glm::vec3(50.0f, 0.0f, 50.0f)};
  desc.shape_key = "test:ground";
  return desc;
}

// Steps `world` for `count` fixed steps.
void StepWorld(PhysicsWorld& world, int count) {
  for (int i = 0; i < count; ++i) {
    world.Update(kStep);
  }
}

TEST_CASE("free fall matches the discrete semi-implicit integration") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  const core::Result<BodyId, PhysicsError> sphere = world.CreateBody(
      CubeDesc(scene::ShapeType::kSphere, scene::MotionType::kDynamic,
               glm::vec3(0.0f, 100.0f, 0.0f)));
  REQUIRE(sphere.ok());

  StepWorld(world, 60);

  // Semi-implicit Euler: v_n = n*g*dt and y_n = y_0 - g*dt^2 * n(n+1)/2 after
  // n steps. The accumulator is ~0 after whole steps, so the interpolated pose
  // is the one from step n - 1 = 59 (rendering lags the simulation by one
  // step): 4.8176 m, 1.8 % below the continuous 4.905 m. The step-60 pose
  // would be 4.9905 m (1.7 % above); the velocity pins down n = 60 exactly.
  CHECK(world.LinearVelocity(sphere.value()).y ==
        doctest::Approx(-9.81f).epsilon(1.0e-4));
  const float discrete_drop = 9.81f * kStep * kStep * 59.0f * 60.0f / 2.0f;
  const Pose end_pose = world.InterpolatedPose(sphere.value());
  CHECK(100.0f - end_pose.position.y ==
        doctest::Approx(discrete_drop).epsilon(1.0e-3));
  CHECK(100.0f - end_pose.position.y == doctest::Approx(4.905f).epsilon(0.02));
}

TEST_CASE("impulse sets velocity to impulse over mass") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(kNoGravity);
  const core::Result<BodyId, PhysicsError> body = world.CreateBody(CubeDesc(
      scene::ShapeType::kBox, scene::MotionType::kDynamic, glm::vec3(0.0f)));
  REQUIRE(body.ok());

  world.AddImpulse(body.value(), glm::vec3(20.0f, 0.0f, 0.0f));
  world.Update(kStep);

  const glm::vec3 velocity = world.LinearVelocity(body.value());
  CHECK(velocity.x == doctest::Approx(2.0f).epsilon(1.0e-3));  // 20 N*s / 10 kg
  CHECK(velocity.y == doctest::Approx(0.0f).epsilon(1.0e-3));
}

TEST_CASE("force re-applied every step integrates to F over m times t") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(kNoGravity);
  const core::Result<BodyId, PhysicsError> body = world.CreateBody(CubeDesc(
      scene::ShapeType::kBox, scene::MotionType::kDynamic, glm::vec3(0.0f)));
  REQUIRE(body.ok());

  for (int i = 0; i < 60; ++i) {
    world.AddForce(body.value(), glm::vec3(0.0f, 50.0f, 0.0f));
    world.Update(kStep);
  }

  CHECK(world.LinearVelocity(body.value()).y ==
        doctest::Approx(5.0f).epsilon(0.01));  // 50 N / 10 kg * 1 s
}

TEST_CASE("a force is applied for one step only") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(kNoGravity);
  const core::Result<BodyId, PhysicsError> body = world.CreateBody(CubeDesc(
      scene::ShapeType::kBox, scene::MotionType::kDynamic, glm::vec3(0.0f)));
  REQUIRE(body.ok());

  world.AddForce(body.value(), glm::vec3(0.0f, 600.0f, 0.0f));
  StepWorld(world, 10);

  CHECK(world.LinearVelocity(body.value()).y ==
        doctest::Approx(1.0f).epsilon(0.01));  // 600 / 10 * (1/60), not 10x
}

TEST_CASE("a box dropped on the ground comes to rest and sleeps") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  REQUIRE(world.CreateBody(GroundDesc()).ok());
  const core::Result<BodyId, PhysicsError> box = world.CreateBody(
      CubeDesc(scene::ShapeType::kBox, scene::MotionType::kDynamic,
               glm::vec3(0.0f, 3.0f, 0.0f)));
  REQUIRE(box.ok());

  StepWorld(world, 180);  // 3 s

  CHECK(world.InterpolatedPose(box.value()).position.y ==
        doctest::Approx(0.5f).epsilon(0.04));  // +-0.02 m
  const PhysicsStats stats = world.Stats();
  CHECK(stats.bodies == 2);
  CHECK(stats.active_bodies == 0);
  CHECK(world.IsDynamic(box.value()));
}

TEST_CASE("the ground plane keeps its surface as the top face") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  const core::Result<BodyId, PhysicsError> ground =
      world.CreateBody(GroundDesc());
  REQUIRE(ground.ok());

  const std::optional<RayHit> hit = world.CastRay(
      glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 10.0f);

  REQUIRE(hit.has_value());
  CHECK(hit->point.y == doctest::Approx(0.0f).epsilon(1.0e-4).scale(1.0));
  CHECK_FALSE(world.IsDynamic(ground.value()));
}

TEST_CASE("slicing the same time differently gives the same simulation") {
  PhysicsRuntime runtime;
  PhysicsWorld whole(runtime);
  PhysicsWorld halves(runtime);
  std::optional<BodyId> whole_box;
  std::optional<BodyId> halves_box;
  for (PhysicsWorld* world : {&whole, &halves}) {
    world->SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    REQUIRE(world->CreateBody(GroundDesc()).ok());
    const core::Result<BodyId, PhysicsError> box = world->CreateBody(
        CubeDesc(scene::ShapeType::kBox, scene::MotionType::kDynamic,
                 glm::vec3(0.0f, 2.0f, 0.0f)));
    REQUIRE(box.ok());
    (world == &whole ? whole_box : halves_box) = box.value();
  }

  for (int i = 0; i < 60; ++i) {
    whole.Update(kStep);          // One step per call.
    halves.Update(kStep / 2.0f);  // One step per two calls.
    halves.Update(kStep / 2.0f);
  }

  const Pose pose_whole = whole.InterpolatedPose(*whole_box);
  const Pose pose_halves = halves.InterpolatedPose(*halves_box);
  CHECK(pose_whole.position.y ==
        doctest::Approx(pose_halves.position.y).epsilon(1.0e-6));
  CHECK(pose_whole.position.x == doctest::Approx(pose_halves.position.x));
}

TEST_CASE("an Update never runs more than four steps and drops the backlog") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(glm::vec3(0.0f, -10.0f, 0.0f));
  const core::Result<BodyId, PhysicsError> body = world.CreateBody(
      CubeDesc(scene::ShapeType::kSphere, scene::MotionType::kDynamic,
               glm::vec3(0.0f, 100.0f, 0.0f)));
  REQUIRE(body.ok());

  world.Update(1.0f);  // Clamped to 0.25 s, 15 steps due, 4 run.

  CHECK(world.LinearVelocity(body.value()).y ==
        doctest::Approx(-10.0f * 4.0f * kStep).epsilon(1.0e-4));
  world.Update(0.0f);  // The backlog was dropped, nothing is owed.
  CHECK(world.LinearVelocity(body.value()).y ==
        doctest::Approx(-10.0f * 4.0f * kStep).epsilon(1.0e-4));
}

TEST_CASE("interpolation lies between the last two step poses") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  const core::Result<BodyId, PhysicsError> body = world.CreateBody(
      CubeDesc(scene::ShapeType::kSphere, scene::MotionType::kDynamic,
               glm::vec3(0.0f, 100.0f, 0.0f)));
  REQUIRE(body.ok());
  StepWorld(world, 5);
  const float y_before = world.InterpolatedPose(body.value()).position.y;

  world.Update(1.5f * kStep);  // One step, half a step left over.
  const float y_after_step = world.InterpolatedPose(body.value()).position.y;
  world.Update(0.5f * kStep);  // Completes the second step.
  const float y_after_two = world.InterpolatedPose(body.value()).position.y;

  // Falling: y decreases. The mid pose is between step 5 and step 6 poses.
  CHECK(y_after_step < y_before);
  CHECK(y_after_step > y_after_two);
}

TEST_CASE("a ray hits the top face of a box") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  const core::Result<BodyId, PhysicsError> box = world.CreateBody(
      CubeDesc(scene::ShapeType::kBox, scene::MotionType::kStatic,
               glm::vec3(0.0f, 0.5f, 0.0f)));
  REQUIRE(box.ok());

  const std::optional<RayHit> hit = world.CastRay(
      glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, -2.0f, 0.0f), 10.0f);

  REQUIRE(hit.has_value());
  CHECK(hit->body == box.value());
  CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(1.0e-3));
  CHECK(hit->point.y == doctest::Approx(1.0f).epsilon(1.0e-3));
  CHECK(hit->fraction == doctest::Approx(4.0f / 10.0f).epsilon(1.0e-3));

  CHECK_FALSE(world
                  .CastRay(glm::vec3(5.0f, 5.0f, 0.0f),
                           glm::vec3(0.0f, -1.0f, 0.0f), 10.0f)
                  .has_value());
  CHECK_FALSE(world
                  .CastRay(glm::vec3(0.0f, 5.0f, 0.0f),
                           glm::vec3(0.0f, -1.0f, 0.0f), 2.0f)
                  .has_value());
}

TEST_CASE("a dynamic triangle mesh is rejected") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  const asset::MeshData cube = asset::MakeCube(glm::vec3(1.0f));
  BodyDesc desc = CubeDesc(scene::ShapeType::kTriangleMesh,
                           scene::MotionType::kDynamic, glm::vec3(0.0f));
  desc.mesh = &cube;

  const core::Result<BodyId, PhysicsError> result = world.CreateBody(desc);

  REQUIRE_FALSE(result.ok());
  CHECK(result.error() == PhysicsError::kDynamicTriangleMesh);
}

TEST_CASE("hull and mesh shapes need mesh data") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  const BodyDesc hull = CubeDesc(scene::ShapeType::kConvexHull,
                                 scene::MotionType::kDynamic, glm::vec3(0.0f));
  const BodyDesc mesh = CubeDesc(scene::ShapeType::kTriangleMesh,
                                 scene::MotionType::kStatic, glm::vec3(0.0f));

  const core::Result<BodyId, PhysicsError> hull_result = world.CreateBody(hull);
  const core::Result<BodyId, PhysicsError> mesh_result = world.CreateBody(mesh);

  REQUIRE_FALSE(hull_result.ok());
  CHECK(hull_result.error() == PhysicsError::kInvalidShape);
  REQUIRE_FALSE(mesh_result.ok());
  CHECK(mesh_result.error() == PhysicsError::kInvalidShape);
}

TEST_CASE("a static triangle mesh catches a falling sphere") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
  const asset::MeshData plane = asset::MakePlane(20.0f, glm::vec3(1.0f));
  BodyDesc ground = GroundDesc();
  ground.body.shape = scene::ShapeType::kTriangleMesh;
  ground.mesh = &plane;
  ground.shape_key = "builtin:plane";
  REQUIRE(world.CreateBody(ground).ok());
  const core::Result<BodyId, PhysicsError> ball = world.CreateBody(
      CubeDesc(scene::ShapeType::kSphere, scene::MotionType::kDynamic,
               glm::vec3(0.0f, 2.0f, 0.0f)));
  REQUIRE(ball.ok());

  StepWorld(world, 180);

  CHECK(world.InterpolatedPose(ball.value()).position.y ==
        doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("a convex hull of the teapot builds and stays put without gravity") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  world.SetGravity(kNoGravity);
  const std::string path = std::string(BLUE_SAMPLES_DIR) + "/teapot.obj";
  const core::Result<asset::MeshData, asset::ImportError> teapot =
      asset::ImportModel(path, asset::ImportOptions{});
  REQUIRE(teapot.ok());
  BodyDesc desc;
  desc.body.shape = scene::ShapeType::kConvexHull;
  desc.pose.position = glm::vec3(1.0f, 2.0f, 3.0f);
  desc.bounds = teapot.value().bounds;
  desc.mesh = &teapot.value();
  desc.shape_key = path;

  const core::Result<BodyId, PhysicsError> body = world.CreateBody(desc);
  REQUIRE(body.ok());
  StepWorld(world, 30);

  const Pose pose = world.InterpolatedPose(body.value());
  CHECK(pose.position.x == doctest::Approx(1.0f).epsilon(1.0e-5));
  CHECK(pose.position.y == doctest::Approx(2.0f).epsilon(1.0e-5));
  CHECK(pose.position.z == doctest::Approx(3.0f).epsilon(1.0e-5));
}

TEST_CASE("scale resizes the shape and equal keys share work") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  BodyDesc small = CubeDesc(scene::ShapeType::kBox, scene::MotionType::kStatic,
                            glm::vec3(0.0f, 0.5f, 0.0f));
  BodyDesc big = small;
  big.scale = glm::vec3(4.0f);
  big.pose.position = glm::vec3(10.0f, 2.0f, 0.0f);
  REQUIRE(world.CreateBody(small).ok());
  REQUIRE(world.CreateBody(big).ok());

  const std::optional<RayHit> hit = world.CastRay(
      glm::vec3(10.0f, 10.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 20.0f);

  REQUIRE(hit.has_value());
  CHECK(hit->point.y == doctest::Approx(4.0f).epsilon(1.0e-3));
}

TEST_CASE("invalid bodies are rejected") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  BodyDesc zero_mass = CubeDesc(scene::ShapeType::kBox,
                                scene::MotionType::kDynamic, glm::vec3(0.0f));
  zero_mass.body.mass = 0.0f;
  BodyDesc zero_scale = CubeDesc(scene::ShapeType::kBox,
                                 scene::MotionType::kStatic, glm::vec3(0.0f));
  zero_scale.scale = glm::vec3(0.0f, 1.0f, 1.0f);

  CHECK(world.CreateBody(zero_mass).error() == PhysicsError::kInvalidShape);
  CHECK(world.CreateBody(zero_scale).error() == PhysicsError::kInvalidShape);
  CHECK(world.Stats().bodies == 0);
}

TEST_CASE("removing bodies updates the stats and invalidates handles") {
  PhysicsRuntime runtime;
  PhysicsWorld world(runtime);
  const core::Result<BodyId, PhysicsError> first = world.CreateBody(CubeDesc(
      scene::ShapeType::kBox, scene::MotionType::kDynamic, glm::vec3(0.0f)));
  REQUIRE(first.ok());
  REQUIRE(world.CreateBody(GroundDesc()).ok());
  CHECK(world.Stats().bodies == 2);

  world.RemoveBody(first.value());
  world.RemoveBody(first.value());  // Stale handle: ignored.
  CHECK(world.Stats().bodies == 1);
  CHECK_FALSE(world.IsDynamic(first.value()));

  world.RemoveAllBodies();
  CHECK(world.Stats().bodies == 0);
}

TEST_CASE("the runtime can be recreated") {
  for (int round = 0; round < 3; ++round) {
    PhysicsRuntime runtime;
    PhysicsWorld world(runtime);
    CHECK(world.CreateBody(GroundDesc()).ok());
  }
}

}  // namespace
}  // namespace blue::physics
