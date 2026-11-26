#include <doctest/doctest.h>

#include <cstddef>
#include <type_traits>
#include <vector>

#include "weapon/firearm.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kTick = 1.0f / 60.0f;

static_assert(std::is_trivially_copyable_v<FirearmState>);

bool SameEvents(const FirearmEvents& a, const FirearmEvents& b) {
  return a.shots == b.shots && a.shot_age == b.shot_age &&
         a.dry_fire == b.dry_fire && a.reload_started == b.reload_started &&
         a.reload_finished == b.reload_finished &&
         a.fire_mode_changed == b.fire_mode_changed;
}

// Events and final state of running `inputs` on `firearm`, one per tick.
struct Run {
  std::vector<FirearmEvents> events;
  FirearmState end;
};

Run Play(Firearm& firearm, const std::vector<FirearmInput>& inputs) {
  Run run;
  for (const FirearmInput& input : inputs) {
    run.events.push_back(firearm.Update(input, kTick));
  }
  run.end = firearm.SaveState();
  return run;
}

void CheckSameRun(const Run& a, const Run& b) {
  REQUIRE(a.events.size() == b.events.size());
  for (size_t i = 0; i < a.events.size(); ++i) {
    CAPTURE(i);
    CHECK(SameEvents(a.events[i], b.events[i]));
  }
  CHECK(a.end == b.end);
}

// Saves `firearm`, runs `inputs`, restores and runs them again; then runs
// them on a fresh firearm of the same spec restored to the saved state.
// All three runs must match exactly.
void CheckReplay(Firearm& firearm, const std::vector<FirearmInput>& inputs) {
  const FirearmState saved = firearm.SaveState();
  const Run first = Play(firearm, inputs);
  firearm.RestoreState(saved);
  CHECK(firearm.SaveState() == saved);
  const Run second = Play(firearm, inputs);
  CheckSameRun(first, second);

  Firearm other(firearm.spec());
  other.RestoreState(saved);
  const Run third = Play(other, inputs);
  CheckSameRun(first, third);
}

std::vector<FirearmInput> Repeat(const FirearmInput& input, int count) {
  return std::vector<FirearmInput>(static_cast<size_t>(count), input);
}

TEST_CASE("a restored firearm replays a tactical reload identically") {
  Firearm rifle(M16Spec());
  (void)rifle.Update({.trigger_held = true}, kTick);  // one shot
  (void)rifle.Update({}, kTick);
  (void)rifle.Update({.reload_pressed = true}, kTick);
  for (int i = 0; i < 40; ++i) {  // ~0.67 s: past the magazine pull
    (void)rifle.Update({}, kTick);
  }
  REQUIRE(rifle.reloading());
  REQUIRE_FALSE(rifle.empty_reload());
  REQUIRE_FALSE(rifle.magazine_inserted());
  // Through the seating and the end of the reload, then fire.
  std::vector<FirearmInput> inputs = Repeat({}, 120);
  inputs.push_back({.trigger_held = true});
  CheckReplay(rifle, inputs);
}

TEST_CASE("a restored firearm replays an empty reload identically") {
  Firearm pistol(Glock17Spec());
  // Pull for two ticks, release for two (longer than the 55 ms cycle), until
  // the slide locks back after 18 shots.
  for (int i = 0; i < 100; ++i) {
    (void)pistol.Update({.trigger_held = i % 4 < 2}, kTick);
  }
  REQUIRE(pistol.bolt_locked_back());
  (void)pistol.Update({.reload_pressed = true}, kTick);
  for (int i = 0; i < 60; ++i) {
    (void)pistol.Update({}, kTick);
  }
  REQUIRE(pistol.reloading());
  REQUIRE(pistol.empty_reload());
  std::vector<FirearmInput> inputs = Repeat({}, 150);
  inputs.push_back({.trigger_held = true});
  CheckReplay(pistol, inputs);
}

TEST_CASE("a restore between automatic shots keeps the cyclic timing") {
  Firearm rifle(M16Spec());
  (void)rifle.Update({.fire_mode_pressed = true}, kTick);
  REQUIRE(rifle.fire_mode() == FireMode::kAuto);
  // 800 rpm fires every 75 ms: two ticks in, the bolt is mid-cycle.
  int shots = 0;
  for (int i = 0; i < 6; ++i) {
    shots += rifle.Update({.trigger_held = true}, kTick).shots;
  }
  REQUIRE(shots >= 2);
  REQUIRE(rifle.SaveState().cycle_remaining > 0.0f);
  // The trigger stays held across the restore.
  CheckReplay(rifle, Repeat({.trigger_held = true}, 30));
}

TEST_CASE("a held semi trigger stays used across a restore") {
  Firearm rifle(M16Spec());
  REQUIRE(rifle.Update({.trigger_held = true}, kTick).shots == 1);
  REQUIRE(rifle.SaveState().pull_used);
  CheckReplay(rifle, Repeat({.trigger_held = true}, 20));
  Firearm fresh(M16Spec());
  fresh.RestoreState(rifle.SaveState());
  CHECK(fresh.Update({.trigger_held = true}, kTick).shots == 0);
}

TEST_CASE("the state holds what the accessors show") {
  Firearm rifle(M16Spec());
  (void)rifle.Update({.trigger_held = true}, kTick);
  const FirearmState state = rifle.SaveState();
  CHECK(state.magazine == rifle.rounds_in_magazine());
  CHECK(state.chambered == rifle.round_chambered());
  CHECK(state.magazine_inserted);
  CHECK(state.fire_mode == rifle.fire_mode());
  CHECK(state.spare_count == rifle.spare_magazines().size());
  CHECK(state.spares_dropped == 0);
  for (size_t i = 0; i < state.spare_count; ++i) {
    CHECK(state.spares[i] == rifle.spare_magazines()[i]);
  }
}

TEST_CASE("more spares than the state holds are counted as dropped") {
  FirearmSpec spec = M16Spec();
  spec.spare_magazines = kMaxSavedSpareMagazines + 3;
  const Firearm rifle(spec);
  const FirearmState state = rifle.SaveState();
  CHECK(state.spare_count == kMaxSavedSpareMagazines);
  CHECK(state.spares_dropped == 3);
  Firearm restored(spec);
  restored.RestoreState(state);
  CHECK(restored.spare_magazines().size() ==
        static_cast<size_t>(kMaxSavedSpareMagazines));
}

TEST_CASE("states compare exactly or within the timer tolerance") {
  Firearm rifle(M16Spec());
  (void)rifle.Update({.trigger_held = true}, kTick);
  (void)rifle.Update({.reload_pressed = true}, kTick);
  const FirearmState a = rifle.SaveState();
  FirearmState b = a;
  CHECK(a == b);
  CHECK(NearlyEqual(a, b));
  b.reload_elapsed += 0.5f * kStateTimeTolerance;
  CHECK_FALSE(a == b);
  CHECK(NearlyEqual(a, b));
  b.reload_elapsed += kStateTimeTolerance;
  CHECK_FALSE(NearlyEqual(a, b));
  b = a;
  b.cycle_remaining += 2.0f * kStateTimeTolerance;
  CHECK_FALSE(NearlyEqual(a, b));
  b = a;
  b.magazine -= 1;
  CHECK_FALSE(NearlyEqual(a, b));
  b = a;
  b.spares[0] -= 1;
  CHECK_FALSE(NearlyEqual(a, b));
}

}  // namespace
}  // namespace blue::weapon
