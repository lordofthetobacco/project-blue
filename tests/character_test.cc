#include "physics/character.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <memory>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "scene/scene.h"

namespace blue::physics {
namespace {

constexpr float kStep = 1.0f / 60.0f;
constexpr float kGravity = -9.81f;

// Returns a static box with the given local bounds placed at `position`.
BodyDesc StaticBox(glm::vec3 min, glm::vec3 max, glm::vec3 position,
                   const char* key) {
  BodyDesc desc;
  desc.body.shape = scene::ShapeType::kBox;
  desc.body.motion = scene::MotionType::kStatic;
  desc.pose.position = position;
  desc.bounds = asset::Bounds{min, max};
  desc.shape_key = key;
  return desc;
}

// A flat 100 x 100 m ground plane with its surface at y = 0.
BodyDesc GroundDesc() {
  return StaticBox(glm::vec3(-50.0f, 0.0f, -50.0f),
                   glm::vec3(50.0f, 0.0f, 50.0f), glm::vec3(0.0f),
                   "test:ground");
}

// A world with gravity and the ground plane, plus a character helper.
struct Fixture {
  PhysicsRuntime runtime;
  PhysicsWorld world{runtime};

  Fixture() {
    world.SetGravity(glm::vec3(0.0f, kGravity, 0.0f));
    REQUIRE(world.CreateBody(GroundDesc()).ok());
  }

  std::unique_ptr<Character> Spawn(glm::vec3 feet = glm::vec3(0.0f, 0.3f, 0.0f),
                                   CharacterDesc desc = {}) {
    core::Result<std::unique_ptr<Character>, PhysicsError> created =
        Character::Create(world, desc, feet);
    REQUIRE(created.ok());
    return std::move(created.value());
  }

  void Step(int count) {
    for (int i = 0; i < count; ++i) {
      world.Update(kStep);
    }
  }

  // Steps until the character reports ground (or `max_steps` pass).
  void Settle(const Character& character, int max_steps = 120) {
    for (int i = 0; i < max_steps && !character.IsOnGround(); ++i) {
      world.Update(kStep);
    }
  }
};

bool Near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

TEST_CASE("Create rejects inconsistent dimensions") {
  Fixture f;
  CharacterDesc desc;
  desc.crouch_height = 2.0f;  // Taller than standing.
  CHECK_FALSE(Character::Create(f.world, desc, glm::vec3(0.0f)).ok());
  desc = CharacterDesc{};
  desc.radius = 0.7f;  // 2 * radius exceeds the crouch height.
  CHECK_FALSE(Character::Create(f.world, desc, glm::vec3(0.0f)).ok());
  desc = CharacterDesc{};
  desc.radius = 0.0f;
  const auto zero_radius = Character::Create(f.world, desc, glm::vec3(0.0f));
  REQUIRE_FALSE(zero_radius.ok());
  CHECK(zero_radius.error() == PhysicsError::kInvalidShape);
}

TEST_CASE("a character dropped above the ground settles standing on it") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  CHECK(character->height() == doctest::Approx(1.8f));
  CHECK_FALSE(character->crouching());
  f.Step(30);
  CHECK(character->IsOnGround());
  CHECK(character->ground_state() == GroundState::kOnGround);
  CHECK(Near(character->position().y, 0.0f, 0.05f));
  CHECK(Near(character->velocity().x, 0.0f, 1.0e-3f));
  CHECK(Near(character->velocity().z, 0.0f, 1.0e-3f));
  // The character is not a body.
  CHECK(f.world.Stats().bodies == 1);
}

TEST_CASE("desired velocity moves the character at that speed") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  character->SetDesiredVelocity(glm::vec3(2.0f, 5.0f, 0.0f));  // y ignored
  const glm::vec3 start = character->position();
  f.Step(60);
  const glm::vec3 moved = character->position() - start;
  CHECK(Near(moved.x, 2.0f, 0.1f));
  CHECK(Near(moved.z, 0.0f, 0.01f));
  CHECK(Near(moved.y, 0.0f, 0.05f));
  CHECK(character->IsOnGround());
  CHECK(Near(character->velocity().x, 2.0f, 0.05f));

  character->SetDesiredVelocity(glm::vec3(0.0f));
  f.Step(5);
  CHECK(Near(character->velocity().x, 0.0f, 1.0e-3f));
}

TEST_CASE("a jump leaves the ground, peaks and lands") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  REQUIRE(character->IsOnGround());

  character->RequestJump(5.0f);
  int steps_to_leave = 0;
  while (character->IsOnGround() && steps_to_leave < 10) {
    f.Step(1);
    ++steps_to_leave;
  }
  CHECK(steps_to_leave <= 3);
  CHECK(character->velocity().y > 4.0f);

  float peak = 0.0f;
  int steps = 0;
  while (!character->IsOnGround() && steps < 180) {
    f.Step(1);
    peak = std::max(peak, character->position().y);
    ++steps;
  }
  // v^2 / (2 g) = 1.27 m, minus a step of integration slack.
  CHECK(peak > 1.15f);
  CHECK(peak < 1.4f);
  CHECK(character->IsOnGround());
  CHECK(steps < 120);  // Landed within 2 s.
  CHECK(Near(character->position().y, 0.0f, 0.05f));
}

TEST_CASE("a jump requested in the air is ignored") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  character->RequestJump(5.0f);
  f.Step(3);
  REQUIRE_FALSE(character->IsOnGround());
  const float before = character->velocity().y;
  character->RequestJump(5.0f);
  f.Step(1);
  // Only gravity acted; the second request did not add speed.
  CHECK(character->velocity().y < before);
  float peak = 0.0f;
  for (int i = 0; i < 90; ++i) {
    f.Step(1);
    peak = std::max(peak, character->position().y);
  }
  CHECK(peak < 1.4f);
  // The buffered request expired in the air and did not fire on landing.
  CHECK(character->IsOnGround());
}

TEST_CASE("a jump pressed just before landing still fires") {
  Fixture f;
  // Falling 0.25 m takes ~0.23 s = ~14 steps.
  std::unique_ptr<Character> character =
      f.Spawn(glm::vec3(0.0f, 0.25f, 0.0f));
  f.Step(10);
  REQUIRE_FALSE(character->IsOnGround());
  // Requested ~4 steps before landing: inside the 8-step buffer.
  character->RequestJump(5.0f);
  f.Step(8);
  CHECK(character->velocity().y > 3.0f);
}

TEST_CASE("crouching shrinks the capsule and standing up needs headroom") {
  Fixture f;
  // Ceiling slab whose underside is at 1.5 m: too low to stand under.
  REQUIRE(f.world
              .CreateBody(StaticBox(glm::vec3(-1.0f, 0.0f, -1.0f),
                                    glm::vec3(1.0f, 0.2f, 1.0f),
                                    glm::vec3(0.0f, 1.5f, 0.0f),
                                    "test:ceiling"))
              .ok());
  std::unique_ptr<Character> character =
      f.Spawn(glm::vec3(5.0f, 0.3f, 0.0f));
  f.Settle(*character);
  REQUIRE(character->IsOnGround());

  CHECK(character->SetCrouching(true));
  CHECK(character->crouching());
  CHECK(character->height() == doctest::Approx(1.2f));
  f.Step(5);
  CHECK(character->IsOnGround());

  // Walk under the slab while crouched.
  character->SetDesiredVelocity(glm::vec3(-2.0f, 0.0f, 0.0f));
  f.Step(150);
  character->SetDesiredVelocity(glm::vec3(0.0f));
  f.Step(5);
  REQUIRE(Near(character->position().x, 0.0f, 0.3f));
  CHECK(character->IsOnGround());

  CHECK_FALSE(character->SetCrouching(false));
  CHECK(character->crouching());
  CHECK(character->height() == doctest::Approx(1.2f));

  // Walk back out and stand up.
  character->SetDesiredVelocity(glm::vec3(2.0f, 0.0f, 0.0f));
  f.Step(150);
  character->SetDesiredVelocity(glm::vec3(0.0f));
  f.Step(5);
  CHECK(character->SetCrouching(false));
  CHECK_FALSE(character->crouching());
  CHECK(character->height() == doctest::Approx(1.8f));
  f.Step(5);
  CHECK(character->IsOnGround());
}

TEST_CASE("the character climbs a low step") {
  Fixture f;
  // A 0.3 m high platform from x = 2 onwards.
  REQUIRE(f.world
              .CreateBody(StaticBox(glm::vec3(0.0f, 0.0f, -5.0f),
                                    glm::vec3(10.0f, 0.3f, 5.0f),
                                    glm::vec3(2.0f, 0.0f, 0.0f), "test:step"))
              .ok());
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  character->SetDesiredVelocity(glm::vec3(2.0f, 0.0f, 0.0f));
  f.Step(120);  // 4 m of travel at 2 m/s.
  CHECK(character->position().x > 2.5f);
  CHECK(Near(character->position().y, 0.3f, 0.05f));
  CHECK(character->IsOnGround());
}

TEST_CASE("a wall stops the character") {
  Fixture f;
  // Wall face at x = 3.
  REQUIRE(f.world
              .CreateBody(StaticBox(glm::vec3(0.0f, 0.0f, -5.0f),
                                    glm::vec3(1.0f, 3.0f, 5.0f),
                                    glm::vec3(3.0f, 0.0f, 0.0f), "test:wall"))
              .ok());
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  character->SetDesiredVelocity(glm::vec3(3.0f, 0.0f, 0.0f));
  f.Step(180);
  const glm::vec3 position = character->position();
  CHECK(position.x <= 3.0f - character->desc().radius + 0.05f);
  CHECK(position.x > 2.4f);
  CHECK(Near(position.y, 0.0f, 0.05f));
  CHECK(character->IsOnGround());
}

// Adds a heavy dynamic 1 x 1.5 x 10 m block whose near face is x = 3.
BodyId AddBlock(PhysicsWorld& world, bool blocks_characters) {
  core::Result<std::vector<std::byte>, PhysicsError> cooked = CookShape(
      {.kind = CookedShapeKind::kBox, .half_extents = {0.5f, 0.75f, 5.0f}});
  REQUIRE(cooked.ok());
  core::Result<CookedShape, PhysicsError> shape =
      CookedShape::Restore(cooked.value());
  REQUIRE(shape.ok());
  const core::Result<BodyId, PhysicsError> body = world.CreateBody(
      shape.value(), {.motion = scene::MotionType::kDynamic,
                      .pose = {.position = {3.5f, 0.75f, 0.0f}},
                      .mass = 2000.0f,
                      .friction = 1.0f,
                      .blocks_characters = blocks_characters});
  REQUIRE(body.ok());
  return body.value();
}

TEST_CASE("a body that does not block characters is walked through") {
  for (const bool blocks : {true, false}) {
    CAPTURE(blocks);
    Fixture f;
    const BodyId block = AddBlock(f.world, blocks);
    std::unique_ptr<Character> character = f.Spawn();
    f.Settle(*character);
    character->SetDesiredVelocity(glm::vec3(3.0f, 0.0f, 0.0f));
    f.Step(180);
    if (blocks) {
      CHECK(character->position().x < 3.0f);
    } else {
      CHECK(character->position().x > 5.0f);
      // Not pushed either.
      CHECK(f.world.InterpolatedPose(block).position.x ==
            doctest::Approx(3.5f).epsilon(1.0e-3));
    }
  }
}

TEST_CASE("position interpolates between the last two steps") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  character->SetDesiredVelocity(glm::vec3(2.0f, 0.0f, 0.0f));
  f.Step(10);
  const float at_step = character->position().x;
  f.world.Update(1.5f * kStep);  // One step plus half a step of remainder.
  const float expected = at_step + 2.0f * kStep * 1.5f;
  CHECK(Near(character->position().x, expected, 0.01f));
  f.world.Update(0.5f * kStep);  // Completes the second step: no remainder.
  CHECK(Near(character->position().x, at_step + 2.0f * kStep * 2.0f, 0.01f));
}

TEST_CASE("Teleport moves the character instantly") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  character->Teleport(glm::vec3(10.0f, 2.0f, -4.0f));
  CHECK(character->position() == glm::vec3(10.0f, 2.0f, -4.0f));
  CHECK(glm::length(character->velocity()) == doctest::Approx(0.0f));
  f.Step(60);
  CHECK(character->IsOnGround());
  CHECK(Near(character->position().x, 10.0f, 1.0e-3f));
  CHECK(Near(character->position().z, -4.0f, 1.0e-3f));
  CHECK(Near(character->position().y, 0.0f, 0.05f));
}

TEST_CASE("the character survives a rebuild of the world's bodies") {
  Fixture f;
  std::unique_ptr<Character> character = f.Spawn();
  f.Settle(*character);
  f.world.RemoveAllBodies();
  CHECK(f.world.Stats().bodies == 0);
  REQUIRE(f.world.CreateBody(GroundDesc()).ok());
  character->RefreshContacts();
  f.Step(30);
  CHECK(character->IsOnGround());
  CHECK(Near(character->position().y, 0.0f, 0.05f));
}

TEST_CASE("a character destroyed before its world leaves the world usable") {
  Fixture f;
  {
    std::unique_ptr<Character> character = f.Spawn();
    f.Step(10);
  }
  f.Step(10);
  CHECK(f.world.Stats().bodies == 1);
}

}  // namespace
}  // namespace blue::physics
