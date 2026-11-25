#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "net/prediction.h"
#include "net/user_command_codec.h"
#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "scene/scene.h"
#include "sim/player_sim.h"
#include "sim/player_state.h"
#include "sim/user_command.h"
#include "sim/weapon_table.h"
#include "tests/sim/sim_test_scene.h"
#include "weapon/loadout.h"
#include "weapon/weapon_def.h"
#include "weapon/weapon_item.h"

namespace blue::net {
namespace {

namespace held = sim::buttons::held;

// A floor, a pistol and a rifle, and one player (as in player_sim_test).
struct Fixture {
  physics::PhysicsRuntime runtime;
  physics::PhysicsWorld world{runtime, {.worker_threads = 0}};
  weapon::WeaponLibrary library;
  sim::WeaponTable table;
  std::unique_ptr<sim::PlayerSim> player;

  Fixture() {
    physics::BodyDesc floor;
    floor.body.motion = scene::MotionType::kStatic;
    floor.body.shape = scene::ShapeType::kBox;
    floor.pose.position = glm::vec3(0.0f, -0.5f, 0.0f);
    floor.bounds = {.min = glm::vec3(-50.0f, -0.5f, -50.0f),
                    .max = glm::vec3(50.0f, 0.5f, 50.0f)};
    REQUIRE(world.CreateBody(floor).ok());
    const weapon::WeaponDefId pistol = *library.Register(
        1, sim::testing::MakeWeaponPrefab("glock17"), "pistol");
    const weapon::WeaponDefId rifle =
        *library.Register(2, sim::testing::MakeWeaponPrefab("m16"), "rifle");
    const std::vector<sim::WeaponTableEntry> entries = {
        {.name = "prefab:pistol.pbo", .def = pistol},
        {.name = "prefab:rifle.pbo", .def = rifle}};
    auto built = sim::WeaponTable::Build(entries, library);
    REQUIRE(built.ok());
    table = std::move(built).value();
    const std::vector<weapon::WeaponItem> weapons = {
        weapon::MakeFreshWeapon(pistol, library.def(pistol)),
        weapon::MakeFreshWeapon(rifle, library.def(rifle))};
    auto created =
        sim::PlayerSim::Create(world, library, table, 0, glm::vec3(0.0f),
                               camera::LookAngles{}, weapon::Loadout(weapons));
    REQUIRE(created.ok());
    player = std::move(created).value();
  }
};

sim::UserCommand Walk(float yaw) {
  sim::UserCommand command;
  command.held = held::kForward;
  command.yaw = yaw;
  return command;
}

TEST_CASE("prediction stores canonical commands and states by tick") {
  Fixture f;
  Prediction prediction(*f.player, PredictionConfig{});
  REQUIRE(prediction.Reset(f.player->Save(), 99));
  CHECK(prediction.next_tick() == 100);
  sim::UserCommand command = Walk(0.123456789f);
  command.tick = 7;  // Replaced.
  (void)prediction.Step(command);
  CHECK(prediction.newest_tick() == 100);
  const sim::UserCommand* stored = prediction.CommandAt(100);
  REQUIRE(stored != nullptr);
  CHECK(stored->tick == 100);
  sim::UserCommand expected = Canonical(command);
  expected.tick = 100;
  CHECK(*stored == expected);
  REQUIRE(prediction.StateAt(100) != nullptr);
  CHECK_FALSE(sim::NeedsReplay(*prediction.StateAt(100), f.player->Save()));
  CHECK(prediction.CommandAt(101) == nullptr);
  // The ring keeps kHistory ticks.
  for (int i = 0; i < Prediction::kHistory; ++i) {
    (void)prediction.Step(Walk(0.0f));
  }
  CHECK(prediction.CommandAt(100) == nullptr);
  CHECK(prediction.CommandAt(prediction.newest_tick()) != nullptr);
}

TEST_CASE("prediction matches an identical authority without replay") {
  Fixture f;
  Prediction prediction(*f.player, PredictionConfig{});
  REQUIRE(prediction.Reset(f.player->Save(), 0));
  for (int i = 0; i < 20; ++i) {
    (void)prediction.Step(Walk(0.0f));
  }
  const sim::PlayerState at_10 = *prediction.StateAt(10);
  CHECK(prediction.Reconcile(10, at_10) == ReconcileOutcome::kMatched);
  CHECK(prediction.replays() == 0);
  CHECK(prediction.StateAt(10) == nullptr);  // Settled history is dropped.
  CHECK(prediction.Reconcile(10, at_10) == ReconcileOutcome::kIgnored);
  CHECK(prediction.Reconcile(5, at_10) == ReconcileOutcome::kIgnored);
}

TEST_CASE("prediction replays from a correction and smooths it") {
  Fixture f;
  Prediction prediction(*f.player, PredictionConfig{});
  REQUIRE(prediction.Reset(f.player->Save(), 0));
  for (int i = 0; i < 30; ++i) {
    (void)prediction.Step(Walk(0.0f));
  }
  const glm::vec3 before = f.player->feet();
  // The server says the player was 0.5 m to the side at tick 10.
  sim::PlayerState corrected = *prediction.StateAt(10);
  corrected.position.x += 0.5f;
  CHECK(prediction.Reconcile(10, corrected) == ReconcileOutcome::kReplayed);
  CHECK(prediction.replays() == 1);
  CHECK(prediction.replayed_ticks() == 20);
  CHECK(prediction.newest_tick() == 30);
  CHECK(f.player->feet().x == doctest::Approx(before.x + 0.5f).epsilon(0.01));
  // Shown where it was, then fading: half after one half-life.
  const glm::vec3 offset = prediction.visual_offset();
  CHECK(glm::length(f.player->feet() + offset - before) < 1.0e-4f);
  prediction.DecayOffset(0.1f);
  CHECK(glm::length(prediction.visual_offset()) ==
        doctest::Approx(glm::length(offset) * 0.5f).epsilon(0.01));
  for (int i = 0; i < 50; ++i) {
    prediction.DecayOffset(0.1f);
  }
  CHECK(prediction.visual_offset() == glm::vec3(0.0f));
}

TEST_CASE("prediction skips ticks it does not predict") {
  Fixture f;
  Prediction prediction(*f.player, PredictionConfig{});
  REQUIRE(prediction.Reset(f.player->Save(), 10));
  prediction.SkipTo(15);
  CHECK(prediction.next_tick() == 15);
  prediction.SkipTo(12);  // Never backwards.
  CHECK(prediction.next_tick() == 15);
  // Skipped ticks have no prediction to compare.
  CHECK(prediction.Reconcile(13, f.player->Save()) ==
        ReconcileOutcome::kIgnored);
  (void)prediction.Step(Walk(0.0f));
  CHECK(prediction.CommandAt(15) != nullptr);
  CHECK(prediction.CommandAt(15)->tick == 15);
}

TEST_CASE("prediction counts a dead player's respawn timer down") {
  Fixture f;
  f.player->Kill(10);
  Prediction prediction(*f.player, PredictionConfig{});
  REQUIRE(prediction.Reset(f.player->Save(), 0));
  for (int i = 0; i < 3; ++i) {
    (void)prediction.Step(Walk(0.0f));
  }
  CHECK(prediction.StateAt(3)->respawn_ticks == 7);
  CHECK_FALSE(prediction.StateAt(3)->alive);
  // An authority that counted the same way matches.
  CHECK(prediction.Reconcile(3, *prediction.StateAt(3)) ==
        ReconcileOutcome::kMatched);
  for (int i = 0; i < 20; ++i) {
    (void)prediction.Step(Walk(0.0f));
  }
  CHECK(prediction.StateAt(prediction.newest_tick())->respawn_ticks == 0);
  CHECK_FALSE(prediction.StateAt(prediction.newest_tick())->alive);
}

TEST_CASE("prediction snaps large corrections and resets on future ticks") {
  Fixture f;
  Prediction prediction(*f.player, PredictionConfig{});
  REQUIRE(prediction.Reset(f.player->Save(), 0));
  for (int i = 0; i < 10; ++i) {
    (void)prediction.Step(Walk(0.0f));
  }
  sim::PlayerState far = *prediction.StateAt(5);
  far.position += glm::vec3(10.0f, 0.0f, 0.0f);
  CHECK(prediction.Reconcile(5, far) == ReconcileOutcome::kReplayed);
  CHECK(prediction.visual_offset() == glm::vec3(0.0f));
  // A tick newer than every prediction: reset to it.
  sim::PlayerState future = f.player->Save();
  future.position = glm::vec3(3.0f, 0.0f, 3.0f);
  CHECK(prediction.Reconcile(50, future) == ReconcileOutcome::kReset);
  CHECK(prediction.next_tick() == 51);
  CHECK(glm::length(f.player->feet() - future.position) < 1.0e-4f);
}

}  // namespace
}  // namespace blue::net
