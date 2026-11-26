#include <doctest/doctest.h>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/character.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

namespace blue::physics {
namespace {

constexpr float kGravity = -9.81f;

BodyDesc BoxDesc(glm::vec3 min, glm::vec3 max, glm::vec3 position,
                 scene::MotionType motion, const char* key) {
  BodyDesc desc;
  desc.body.shape = scene::ShapeType::kBox;
  desc.body.motion = motion;
  desc.pose.position = position;
  desc.bounds = asset::Bounds{min, max};
  desc.shape_key = key;
  return desc;
}

BodyDesc StaticBox(glm::vec3 min, glm::vec3 max, glm::vec3 position,
                   const char* key) {
  return BoxDesc(min, max, position, scene::MotionType::kStatic, key);
}

// A dynamic 0.5 m cube centred on `position`.
BodyDesc DynamicCube(glm::vec3 position) {
  return BoxDesc(glm::vec3(-0.25f), glm::vec3(0.25f), position,
                 scene::MotionType::kDynamic, "test:cube");
}

// A world with gravity, a ground plane at y = 0, a 0.3 m platform covering
// x in [2, 6], z in [-2, 2], and a slab whose underside is at 1.5 m over
// x in [4, 8], z in [-8, -4].
// `runtime` must outlive the world; only one runtime may exist at a time.
class World {
 public:
  explicit World(const PhysicsRuntime& runtime) : world_(runtime) {
    world_.SetGravity(glm::vec3(0.0f, kGravity, 0.0f));
    REQUIRE(world_
                .CreateBody(StaticBox(glm::vec3(-50.0f, 0.0f, -50.0f),
                                      glm::vec3(50.0f, 0.0f, 50.0f),
                                      glm::vec3(0.0f), "test:ground"))
                .ok());
    REQUIRE(world_
                .CreateBody(StaticBox(
                    glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(4.0f, 0.3f, 2.0f),
                    glm::vec3(2.0f, 0.0f, 0.0f), "test:platform"))
                .ok());
    REQUIRE(world_
                .CreateBody(StaticBox(
                    glm::vec3(-2.0f, 0.0f, -2.0f), glm::vec3(2.0f, 0.2f, 2.0f),
                    glm::vec3(6.0f, 1.5f, -6.0f), "test:ceiling"))
                .ok());
  }

  PhysicsWorld& world() { return world_; }

  BodyId AddCube(glm::vec3 position) {
    core::Result<BodyId, PhysicsError> created =
        world_.CreateBody(DynamicCube(position));
    REQUIRE(created.ok());
    return created.value();
  }

  std::unique_ptr<Character> Spawn(glm::vec3 feet, bool manual_step) {
    CharacterDesc desc;
    desc.manual_step = manual_step;
    core::Result<std::unique_ptr<Character>, PhysicsError> created =
        Character::Create(world_, desc, feet);
    REQUIRE(created.ok());
    return std::move(created.value());
  }

 private:
  PhysicsWorld world_;
};

// Adds a few tumbling cubes that land on the ground and on each other.
std::vector<BodyId> AddCubes(World& world) {
  std::vector<BodyId> cubes;
  for (int i = 0; i < 4; ++i) {
    const float f = static_cast<float>(i);
    const BodyId cube =
        world.AddCube(glm::vec3(-5.0f + 0.2f * f, 1.0f + 0.7f * f, 3.0f));
    world.world().AddImpulseAtPoint(cube, glm::vec3(0.3f, 0.0f, 0.1f * f),
                                    glm::vec3(-5.0f, 1.2f + 0.7f * f, 3.0f));
    cubes.push_back(cube);
  }
  return cubes;
}

void CheckSamePoses(const PhysicsWorld& a, const PhysicsWorld& b,
                    const std::vector<BodyId>& bodies) {
  for (BodyId id : bodies) {
    const BodyState sa = a.GetBodyState(id);
    const BodyState sb = b.GetBodyState(id);
    CHECK(sa.pose.position == sb.pose.position);
    CHECK(sa.pose.rotation == sb.pose.rotation);
    CHECK(sa.linear_velocity == sb.linear_velocity);
  }
}

TEST_CASE("StepOnce N times equals Update over the same time") {
  PhysicsRuntime runtime;
  World stepped(runtime);
  World updated(runtime);
  const std::vector<BodyId> cubes = AddCubes(stepped);
  REQUIRE(AddCubes(updated) == cubes);

  SUBCASE("one step per update") {
    for (int i = 0; i < 120; ++i) {
      stepped.world().StepOnce();
      updated.world().Update(kFixedStepSeconds);
    }
  }
  SUBCASE("several steps per update") {
    for (int i = 0; i < 30; ++i) {
      for (int j = 0; j < 4; ++j) {
        stepped.world().StepOnce();
      }
      updated.world().Update(4.0f * kFixedStepSeconds);
    }
  }
  CheckSamePoses(stepped.world(), updated.world(), cubes);
  // The cubes did move.
  CHECK(stepped.world().GetBodyState(cubes.back()).pose.position.y < 2.0f);
}

TEST_CASE("SetInterpolationAlpha blends between the last two steps") {
  PhysicsRuntime runtime;
  World world(runtime);
  const BodyId cube = world.AddCube(glm::vec3(0.0f, 5.0f, 10.0f));
  world.world().StepOnce();
  const glm::vec3 before = world.world().GetBodyState(cube).pose.position;
  world.world().StepOnce();
  const glm::vec3 after = world.world().GetBodyState(cube).pose.position;
  REQUIRE(after.y < before.y);

  world.world().SetInterpolationAlpha(0.0f);
  CHECK(world.world().InterpolatedPose(cube).position.y ==
        doctest::Approx(before.y));
  world.world().SetInterpolationAlpha(0.5f);
  CHECK(world.world().InterpolatedPose(cube).position.y ==
        doctest::Approx(0.5f * (before.y + after.y)));
  world.world().SetInterpolationAlpha(7.0f);  // Clamped to 1.
  CHECK(world.world().InterpolatedPose(cube).position.y ==
        doctest::Approx(after.y));
}

TEST_CASE("a world with an explicit worker count steps like the default") {
  PhysicsRuntime runtime;
  PhysicsWorld single(runtime, PhysicsWorldOptions{.worker_threads = 0});
  single.SetGravity(glm::vec3(0.0f, kGravity, 0.0f));
  core::Result<BodyId, PhysicsError> cube =
      single.CreateBody(DynamicCube(glm::vec3(0.0f, 3.0f, 0.0f)));
  REQUIRE(cube.ok());
  for (int i = 0; i < 30; ++i) {
    single.StepOnce();
  }
  // Free fall for 0.5 s: about 1.2 m.
  CHECK(single.GetBodyState(cube.value()).pose.position.y ==
        doctest::Approx(3.0f - 0.5f * 9.81f * 0.25f).epsilon(0.05));
}

TEST_CASE("the world leaves a manual-step character alone") {
  PhysicsRuntime runtime;
  World world(runtime);
  std::unique_ptr<Character> character =
      world.Spawn(glm::vec3(0.0f, 2.0f, 0.0f), /*manual_step=*/true);
  character->SetDesiredVelocity(glm::vec3(3.0f, 0.0f, 0.0f));
  for (int i = 0; i < 30; ++i) {
    world.world().StepOnce();
  }
  world.world().Update(0.25f);
  CHECK(character->tick_position() == glm::vec3(0.0f, 2.0f, 0.0f));
  CHECK(character->velocity() == glm::vec3(0.0f));

  character->Step();
  CHECK(character->tick_position().y < 2.0f);
}

TEST_CASE("Step on a world-stepped character does nothing") {
  PhysicsRuntime runtime;
  World world(runtime);
  std::unique_ptr<Character> character =
      world.Spawn(glm::vec3(0.0f, 2.0f, 0.0f), /*manual_step=*/false);
  character->Step();
  CHECK(character->tick_position() == glm::vec3(0.0f, 2.0f, 0.0f));
  world.world().StepOnce();
  CHECK(character->tick_position().y < 2.0f);
}

// One tick of scripted character input.
struct Input {
  glm::vec3 velocity{0.0f};
  bool jump = false;
  bool crouch = false;
};

// Settle, walk up the platform step, jump on it, fall off its far edge, then
// crouch, walk under the slab and try to stand up below it.
Input ScriptedInput(int tick) {
  Input input;
  if (tick >= 60 && tick < 200) {
    input.velocity = glm::vec3(3.0f, 0.0f, 0.0f);
  }
  input.jump = tick == 100;
  if (tick >= 240) {
    input.crouch = tick < 440;
  }
  if (tick >= 240 && tick < 420) {
    input.velocity = glm::vec3(0.0f, 0.0f, -2.0f);
  }
  return input;
}

void Apply(Character& character, const Input& input) {
  character.SetDesiredVelocity(input.velocity);
  if (input.jump) {
    character.RequestJump(5.0f);
  }
  (void)character.SetCrouching(input.crouch);
}

TEST_CASE("a manually stepped character matches a world-stepped twin") {
  PhysicsRuntime runtime;
  World auto_world(runtime);
  World manual_world(runtime);
  std::unique_ptr<Character> automatic =
      auto_world.Spawn(glm::vec3(0.0f, 0.3f, 0.0f), /*manual_step=*/false);
  std::unique_ptr<Character> manual =
      manual_world.Spawn(glm::vec3(0.0f, 0.3f, 0.0f), /*manual_step=*/true);

  bool climbed = false;
  bool jumped = false;
  bool blocked_under_slab = false;
  for (int tick = 0; tick < 480; ++tick) {
    const Input input = ScriptedInput(tick);
    Apply(*automatic, input);
    Apply(*manual, input);
    auto_world.world().StepOnce();
    manual->Step();
    manual_world.world().StepOnce();

    REQUIRE(manual->tick_position() == automatic->tick_position());
    REQUIRE(manual->velocity() == automatic->velocity());
    REQUIRE(manual->ground_state() == automatic->ground_state());
    REQUIRE(manual->crouching() == automatic->crouching());
    climbed = climbed || automatic->tick_position().y > 0.25f;
    jumped = jumped || automatic->tick_position().y > 1.0f;
    blocked_under_slab =
        blocked_under_slab || (!input.crouch && automatic->crouching());
  }
  // The script exercised what it is meant to.
  CHECK(climbed);
  CHECK(jumped);
  CHECK(blocked_under_slab);
}

TEST_CASE("a body state round trips") {
  PhysicsRuntime runtime;
  World world(runtime);
  const BodyId cube = world.AddCube(glm::vec3(0.0f, 5.0f, 10.0f));
  world.world().AddImpulseAtPoint(cube, glm::vec3(1.0f, 0.0f, 0.0f),
                                  glm::vec3(0.0f, 5.2f, 10.0f));
  for (int i = 0; i < 10; ++i) {
    world.world().StepOnce();
  }
  const BodyState saved = world.world().GetBodyState(cube);
  CHECK(saved.active);
  CHECK(world.world().IsActive(cube));

  std::vector<BodyState> first;
  for (int i = 0; i < 20; ++i) {
    world.world().StepOnce();
    first.push_back(world.world().GetBodyState(cube));
  }

  world.world().SetBodyState(cube, saved);
  const BodyState restored = world.world().GetBodyState(cube);
  CHECK(restored.pose.position == saved.pose.position);
  CHECK(glm::length(restored.linear_velocity - saved.linear_velocity) < 1e-6f);
  CHECK(glm::length(restored.angular_velocity - saved.angular_velocity) <
        1e-6f);
  CHECK(restored.active);
  // Restoring is a jump, not a motion: no interpolation from the old pose.
  world.world().SetInterpolationAlpha(0.5f);
  CHECK(world.world().InterpolatedPose(cube).position == saved.pose.position);

  // Free fall replays the same way.
  for (int i = 0; i < 20; ++i) {
    world.world().StepOnce();
    const BodyState again = world.world().GetBodyState(cube);
    CHECK(glm::length(again.pose.position - first[i].pose.position) < 1e-5f);
  }

  SUBCASE("an inactive state puts the body to sleep") {
    BodyState asleep = saved;
    asleep.active = false;
    world.world().SetBodyState(cube, asleep);
    CHECK_FALSE(world.world().IsActive(cube));
    CHECK(world.world().GetBodyState(cube).linear_velocity == glm::vec3(0.0f));
  }
  SUBCASE("stale handles are ignored") {
    world.world().RemoveBody(cube);
    world.world().SetBodyState(cube, saved);
    CHECK_FALSE(world.world().IsActive(cube));
    CHECK(world.world().GetBodyState(cube).pose.position == glm::vec3(0.0f));
  }
}

TEST_CASE("a kinematic body reaches its target and reports its velocity") {
  PhysicsRuntime runtime;
  World world(runtime);
  const BodyId cube = world.AddCube(glm::vec3(0.0f, 5.0f, 10.0f));
  world.world().SetKinematic(cube, true);
  CHECK_FALSE(world.world().IsDynamic(cube));

  const Pose target{glm::vec3(1.0f, 5.0f, 10.0f),
                    glm::angleAxis(0.1f, glm::vec3(0.0f, 1.0f, 0.0f))};
  world.world().MoveKinematic(cube, target, kFixedStepSeconds);
  world.world().StepOnce();
  const BodyState state = world.world().GetBodyState(cube);
  CHECK(glm::length(state.pose.position - target.position) < 1e-4f);
  CHECK(std::abs(glm::dot(state.pose.rotation, target.rotation)) ==
        doctest::Approx(1.0f));
  CHECK(world.world().LinearVelocity(cube).x ==
        doctest::Approx(1.0f / kFixedStepSeconds).epsilon(1e-3));
  CHECK(world.world().LinearVelocity(cube).y == doctest::Approx(0.0f));

  world.world().SetKinematic(cube, false);
  CHECK(world.world().IsDynamic(cube));

  SUBCASE("non-kinematic bodies ignore MoveKinematic") {
    const glm::vec3 before = world.world().GetBodyState(cube).pose.position;
    world.world().MoveKinematic(cube, Pose{glm::vec3(50.0f), {}},
                                kFixedStepSeconds);
    CHECK(world.world().GetBodyState(cube).pose.position == before);
    CHECK(world.world().LinearVelocity(cube).x < 100.0f);
  }
}

}  // namespace
}  // namespace blue::physics
