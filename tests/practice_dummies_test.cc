// Tests of the offline practice dummies (game/practice_dummies.h): a hit
// dummy gives one ragdoll spawn, its proxy returns after the respawn time.
// Runs on the real rig: kyle.pbo as blue-pack samples cooks it into the
// build's assets.

#include "game/practice_dummies.h"

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <glm/vec3.hpp>

#include "core/path.h"
#include "game/character_animator.h"
#include "game/ragdolls.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "prefab/prefab_file.h"

namespace blue::game {
namespace {

const CharacterRig& Rig() {
  static const CharacterRig* const rig = [] {
    auto loaded = prefab::LoadPrefab(
        core::JoinPath(BLUE_ASSETS_DIR, std::string_view("kyle.pbo")));
    REQUIRE(loaded.ok());
    auto made =
        MakeCharacterRig(loaded.value().hierarchy, loaded.value().sockets);
    REQUIRE(made.ok());
    return new CharacterRig(std::move(made).value());
  }();
  return *rig;
}

constexpr glm::vec3 kFeetA{0.0f, 0.0f, 0.0f};
constexpr glm::vec3 kFeetB{5.0f, 0.0f, 0.0f};
constexpr glm::vec3 kBullet{0.0f, 0.0f, -400.0f};

// A world on the calling thread (one PhysicsRuntime at a time) with two
// standing dummies.
struct Fixture {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime, {.worker_threads = 0}};
  int bodies_before = world.Stats().bodies;
  PracticeDummies dummies{world};

  Fixture() {
    const std::array<DummyPlacement, 2> placements = {
        DummyPlacement{.feet = kFeetA, .tag = 7},
        DummyPlacement{.feet = kFeetB, .yaw = 1.0f, .tag = 8}};
    REQUIRE(dummies.Reset(Rig(), placements) == 2);
  }

  // The body of the proxy of dummy `index`.
  physics::BodyId Proxy(size_t index) const {
    REQUIRE(dummies.dummies()[index].proxy.has_value());
    return *dummies.dummies()[index].proxy;
  }
};

TEST_CASE("dummies stand with a proxy body each") {
  Fixture f;
  CHECK(f.world.Stats().bodies == f.bodies_before + 2);
  for (const PracticeDummy& dummy : f.dummies.dummies()) {
    CHECK(dummy.standing);
    CHECK(dummy.animator.has_value());
  }
  CHECK(f.dummies.dummies()[1].placement.tag == 8);
  // Bullets collide with the proxy: a ray at chest height hits it.
  const auto hit =
      f.world.CastRay(glm::vec3(0.0f, 1.2f, 3.0f), glm::vec3(0, 0, -1), 10.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->body == f.Proxy(0));
  CHECK(f.dummies.IsProxy(hit->body));
}

TEST_CASE("a hit on a proxy takes it away and asks for one ragdoll") {
  Fixture f;
  const physics::BodyId proxy = f.Proxy(0);
  const std::optional<RagdollSpawn> spawn =
      f.dummies.Hit(proxy, glm::vec3(0.0f, 1.2f, 0.3f), kBullet, 0.004f);
  REQUIRE(spawn.has_value());
  REQUIRE(spawn->hit.has_value());
  CHECK(spawn->hit->point == glm::vec3(0.0f, 1.2f, 0.3f));
  CHECK(spawn->hit->velocity == kBullet);
  CHECK(spawn->hit->mass == doctest::Approx(0.004f));
  CHECK(spawn->hit->zone == RagdollZone::kTorso);
  CHECK(spawn->locals.size() == Rig().node_count);
  CHECK(spawn->velocity == glm::vec3(0.0f));
  CHECK(f.world.Stats().bodies == f.bodies_before + 1);
  CHECK_FALSE(f.dummies.dummies()[0].standing);
  CHECK_FALSE(f.dummies.IsProxy(proxy));
  // The same body again is no longer a dummy: no second ragdoll.
  CHECK_FALSE(
      f.dummies.Hit(proxy, glm::vec3(0.0f, 1.2f, 0.3f), kBullet, 0.004f));
  // The ray passes through where the proxy was.
  CHECK_FALSE(
      f.world.CastRay(glm::vec3(0.0f, 1.2f, 3.0f), glm::vec3(0, 0, -1), 10.0f)
          .has_value());
  // The other dummy is untouched.
  CHECK(f.dummies.dummies()[1].standing);
}

TEST_CASE("the zone follows the height of the hit") {
  struct Case {
    float height;  // m above the feet
    RagdollZone zone;
  };
  for (const Case& c :
       {Case{1.7f, RagdollZone::kHead}, Case{1.2f, RagdollZone::kTorso},
        Case{0.3f, RagdollZone::kLegs}}) {
    Fixture f;
    const auto spawn = f.dummies.Hit(
        f.Proxy(1), kFeetB + glm::vec3(0, c.height, 0), kBullet, 0.004f);
    REQUIRE(spawn.has_value());
    CHECK(spawn->hit->zone == c.zone);
  }
}

TEST_CASE("hits on other bodies are not taken") {
  Fixture f;
  // A body that is not a proxy, and a handle nobody owns.
  CHECK_FALSE(f.dummies.Hit(static_cast<physics::BodyId>(0x7fffff),
                            glm::vec3(0.0f, 1.0f, 0.0f), kBullet, 0.004f));
  CHECK(f.world.Stats().bodies == f.bodies_before + 2);
  for (const PracticeDummy& dummy : f.dummies.dummies()) {
    CHECK(dummy.standing);
  }
}

TEST_CASE("a fallen dummy stands up again after the respawn time") {
  Fixture f;
  REQUIRE(
      f.dummies.Hit(f.Proxy(0), glm::vec3(0.0f, 1.2f, 0.3f), kBullet, 0.004f));
  const float dt = 1.0f / 60.0f;
  for (int i = 0; i < 4 * 60; ++i) {  // 4 s: still down
    f.dummies.Update(dt);
  }
  CHECK_FALSE(f.dummies.dummies()[0].standing);
  CHECK(f.world.Stats().bodies == f.bodies_before + 1);
  for (int i = 0; i < 61; ++i) {  // past 5 s
    f.dummies.Update(dt);
  }
  CHECK(f.dummies.dummies()[0].standing);
  CHECK(f.world.Stats().bodies == f.bodies_before + 2);
  const auto hit =
      f.world.CastRay(glm::vec3(0.0f, 1.2f, 3.0f), glm::vec3(0, 0, -1), 10.0f);
  REQUIRE(hit.has_value());
  CHECK(f.dummies.IsProxy(hit->body));
  // It can be shot down again.
  CHECK(f.dummies.Hit(hit->body, hit->point, kBullet, 0.004f).has_value());
}

TEST_CASE("Clear removes every proxy body") {
  Fixture f;
  REQUIRE(
      f.dummies.Hit(f.Proxy(0), glm::vec3(0.0f, 1.2f, 0.3f), kBullet, 0.004f));
  f.dummies.Clear();
  CHECK(f.world.Stats().bodies == f.bodies_before);
  CHECK(f.dummies.dummies().empty());
  f.dummies.Update(10.0f);  // Nothing left to bring back.
  CHECK(f.world.Stats().bodies == f.bodies_before);
}

}  // namespace
}  // namespace blue::game
