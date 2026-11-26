#include <doctest/doctest.h>

#include <memory>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "physics/character.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"

namespace blue::physics {
namespace {

constexpr float kJumpSpeed = 5.0f;
constexpr float kTolerance = 1.0e-5f;

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

// A ground plane at y = 0 and a slab whose underside is at 1.5 m over
// x in [-2, 2], z in [-2, 2], with one manually stepped character.
class Fixture {
 public:
  Fixture() {
    world_.SetGravity(glm::vec3(0.0f, -9.81f, 0.0f));
    REQUIRE(world_
                .CreateBody(StaticBox(glm::vec3(-50.0f, 0.0f, -50.0f),
                                      glm::vec3(50.0f, 0.0f, 50.0f),
                                      glm::vec3(0.0f), "test:ground"))
                .ok());
    REQUIRE(world_
                .CreateBody(StaticBox(
                    glm::vec3(-2.0f, 0.0f, -2.0f), glm::vec3(2.0f, 0.2f, 2.0f),
                    glm::vec3(0.0f, 1.5f, 0.0f), "test:ceiling"))
                .ok());
    CharacterDesc desc;
    desc.manual_step = true;
    core::Result<std::unique_ptr<Character>, PhysicsError> created =
        Character::Create(world_, desc, glm::vec3(5.0f, 0.3f, 0.0f));
    REQUIRE(created.ok());
    character_ = std::move(created.value());
    for (int i = 0; i < 120 && !character_->IsOnGround(); ++i) {
      character_->Step();
    }
    REQUIRE(character_->IsOnGround());
  }

  Character& character() { return *character_; }

  // Steps `count` times with `velocity` as the desired velocity, returning
  // the feet after every step.
  std::vector<glm::vec3> Run(int count, glm::vec3 velocity) {
    std::vector<glm::vec3> feet;
    for (int i = 0; i < count; ++i) {
      character_->SetDesiredVelocity(velocity);
      character_->Step();
      feet.push_back(character_->tick_position());
    }
    return feet;
  }

 private:
  PhysicsRuntime runtime_;
  PhysicsWorld world_{runtime_};
  std::unique_ptr<Character> character_;
};

// Saves, runs `count` steps, restores, runs them again and checks both runs
// agree.
void CheckReplay(Fixture& f, int count, glm::vec3 velocity) {
  const CharacterState saved = f.character().SaveState();
  const bool on_ground = f.character().IsOnGround();
  const std::vector<glm::vec3> first = f.Run(count, velocity);

  f.character().RestoreState(saved);
  CHECK(f.character().tick_position() == saved.position);
  CHECK(f.character().IsOnGround() == on_ground);
  const std::vector<glm::vec3> second = f.Run(count, velocity);
  for (int i = 0; i < count; ++i) {
    INFO("step " << i);
    CHECK(glm::length(second[i] - first[i]) <= kTolerance);
  }
}

TEST_CASE("SaveState captures the jump and stance fields") {
  Fixture f;
  f.character().RequestJump(kJumpSpeed);
  CharacterState state = f.character().SaveState();
  CHECK(state.jump_pending);
  CHECK(state.jump_age == 0);
  CHECK(state.jump_speed == kJumpSpeed);
  CHECK(state.position == f.character().tick_position());

  f.character().Step();  // Launches.
  state = f.character().SaveState();
  CHECK_FALSE(state.jump_pending);
  CHECK(state.jump_cooldown > 0);
  CHECK(state.velocity.y > 4.0f);
  CHECK_FALSE(state.crouching);
}

TEST_CASE("restore and replay reproduce the positions") {
  Fixture f;
  const glm::vec3 walk(1.5f, 0.0f, 0.5f);

  SUBCASE("walking on the ground") { CheckReplay(f, 40, walk); }

  SUBCASE("mid-jump") {
    f.character().RequestJump(kJumpSpeed);
    f.Run(10, walk);
    REQUIRE_FALSE(f.character().IsOnGround());
    CheckReplay(f, 60, walk);  // Lands within the replay.
  }

  SUBCASE("during the jump cooldown") {
    f.character().RequestJump(kJumpSpeed);
    f.Run(1, walk);
    REQUIRE(f.character().SaveState().jump_cooldown > 0);
    CheckReplay(f, 60, walk);
  }

  SUBCASE("with a buffered jump request before landing") {
    f.character().RequestJump(kJumpSpeed);
    f.Run(30, walk);
    f.character().RequestJump(kJumpSpeed);
    REQUIRE(f.character().SaveState().jump_pending);
    CheckReplay(f, 60, walk);
  }

  SUBCASE("crouched under the slab") {
    REQUIRE(f.character().SetCrouching(true));
    f.Run(150, glm::vec3(-2.0f, 0.0f, 0.0f));  // Walk in under the slab.
    REQUIRE(f.character().tick_position().x < 1.0f);
    REQUIRE_FALSE(f.character().SetCrouching(false));
    CheckReplay(f, 40, glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(f.character().crouching());
  }
}

TEST_CASE("RestoreState forces the saved stance") {
  Fixture f;

  SUBCASE("back to standing, even under the slab") {
    const CharacterState standing = f.character().SaveState();
    REQUIRE(f.character().SetCrouching(true));
    f.Run(150, glm::vec3(-2.0f, 0.0f, 0.0f));
    REQUIRE(f.character().tick_position().x < 1.0f);

    CharacterState under_slab = standing;
    under_slab.position = f.character().tick_position();
    f.character().RestoreState(under_slab);
    CHECK_FALSE(f.character().crouching());
    CHECK(f.character().height() == doctest::Approx(1.8f));

    f.character().RestoreState(standing);
    CHECK_FALSE(f.character().crouching());
    CHECK(f.character().tick_position() == standing.position);
  }

  SUBCASE("back to crouching") {
    REQUIRE(f.character().SetCrouching(true));
    const CharacterState crouched = f.character().SaveState();
    REQUIRE(f.character().SetCrouching(false));
    f.character().RestoreState(crouched);
    CHECK(f.character().crouching());
    CHECK(f.character().height() == doctest::Approx(1.2f));
  }
}

TEST_CASE("RestoreState restarts the interpolation") {
  Fixture f;
  const CharacterState saved = f.character().SaveState();
  f.Run(10, glm::vec3(3.0f, 0.0f, 0.0f));
  f.character().RestoreState(saved);
  CHECK(f.character().position() == saved.position);
  CHECK(f.character().velocity() == saved.velocity);
}

}  // namespace
}  // namespace blue::physics
