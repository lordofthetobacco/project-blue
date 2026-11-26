#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "weapon/firearm.h"
#include "weapon/loadout.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kTick = 1.0f / 60.0f;
constexpr WeaponDefId kPistolDef{3};
constexpr WeaponDefId kRifleDef{5};

static_assert(std::is_trivially_copyable_v<LoadoutState>);

WeaponItem Pistol() {
  return {.def = kPistolDef, .firearm = Firearm(Glock17Spec())};
}
WeaponItem Rifle() { return {.def = kRifleDef, .firearm = Firearm(M16Spec())}; }

// A pistol and a rifle, the rifle in hand.
Loadout MakeLoadout() {
  const std::array<WeaponItem, 2> weapons{Pistol(), Rifle()};
  return Loadout(weapons);
}

bool SameEvents(const LoadoutEvents& a, const LoadoutEvents& b) {
  const FirearmEvents& fa = a.firearm;
  const FirearmEvents& fb = b.firearm;
  return fa.shots == fb.shots && fa.shot_age == fb.shot_age &&
         fa.dry_fire == fb.dry_fire && fa.reload_started == fb.reload_started &&
         fa.reload_finished == fb.reload_finished &&
         fa.fire_mode_changed == fb.fire_mode_changed &&
         a.switch_started == b.switch_started &&
         a.weapon_changed == b.weapon_changed;
}

struct Run {
  std::vector<LoadoutEvents> events;
  LoadoutState end;
};

Run Play(Loadout& loadout, const std::vector<LoadoutInput>& inputs) {
  Run run;
  for (const LoadoutInput& input : inputs) {
    run.events.push_back(loadout.Update(input, kTick));
  }
  run.end = loadout.SaveState();
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

// Saves, runs `inputs`, restores, runs them again, and runs them on a fresh
// loadout of the same weapons restored to the saved state: all must match.
void CheckReplay(Loadout& loadout, const std::vector<LoadoutInput>& inputs) {
  const LoadoutState saved = loadout.SaveState();
  const Run first = Play(loadout, inputs);
  CHECK(loadout.RestoreState(saved));
  CHECK(loadout.SaveState() == saved);
  const Run second = Play(loadout, inputs);
  CheckSameRun(first, second);

  Loadout other = MakeLoadout();
  CHECK(other.RestoreState(saved));
  const Run third = Play(other, inputs);
  CheckSameRun(first, third);
}

std::vector<LoadoutInput> Repeat(const LoadoutInput& input, int count) {
  return std::vector<LoadoutInput>(static_cast<size_t>(count), input);
}

const LoadoutInput kTrigger{.firearm = {.trigger_held = true}};

TEST_CASE("a restored loadout replays a reload of the weapon in hand") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(kTrigger, kTick);
  (void)loadout.Update({}, kTick);
  (void)loadout.Update({.firearm = {.reload_pressed = true}}, kTick);
  for (int i = 0; i < 45; ++i) {
    (void)loadout.Update({}, kTick);
  }
  REQUIRE(loadout.active_firearm()->reloading());
  std::vector<LoadoutInput> inputs = Repeat({}, 120);
  inputs.push_back(kTrigger);
  CheckReplay(loadout, inputs);
}

TEST_CASE("a restored loadout replays an empty reload") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update({.firearm = {.fire_mode_pressed = true}}, kTick);
  for (int i = 0; i < 200; ++i) {  // auto: 31 rounds in ~2.3 s
    (void)loadout.Update(kTrigger, kTick);
  }
  REQUIRE(loadout.active_firearm()->bolt_locked_back());
  (void)loadout.Update({.firearm = {.reload_pressed = true}}, kTick);
  for (int i = 0; i < 100; ++i) {
    (void)loadout.Update({}, kTick);
  }
  REQUIRE(loadout.active_firearm()->empty_reload());
  std::vector<LoadoutInput> inputs = Repeat({}, 150);
  inputs.push_back(kTrigger);
  CheckReplay(loadout, inputs);
}

TEST_CASE("a restore in the middle of a weapon switch replays it") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update({.select = WeaponSlot::kPistol}, kTick);
  for (int i = 0; i < 10; ++i) {  // still lowering the rifle
    (void)loadout.Update(kTrigger, kTick);
  }
  REQUIRE(loadout.switching());
  REQUIRE(loadout.active() == WeaponSlot::kRifle);
  REQUIRE(loadout.selected() == WeaponSlot::kPistol);
  REQUIRE(loadout.SaveState().wait_for_release);
  // Trigger held across the restore and through the draw, then released and
  // pulled again.
  std::vector<LoadoutInput> inputs = Repeat(kTrigger, 60);
  inputs.push_back({});
  inputs.push_back(kTrigger);
  CheckReplay(loadout, inputs);
}

TEST_CASE("a restore between automatic shots keeps the cyclic timing") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update({.firearm = {.fire_mode_pressed = true}}, kTick);
  for (int i = 0; i < 6; ++i) {
    (void)loadout.Update(kTrigger, kTick);
  }
  REQUIRE(loadout.active_firearm()->rounds_loaded() < 31);
  CheckReplay(loadout, Repeat(kTrigger, 30));
}

TEST_CASE("the state carries scope settings per slot") {
  Loadout loadout = MakeLoadout();
  REQUIRE(loadout.SetScopeMagnification(4.0f));
  REQUIRE(loadout.SetScopeZero(300.0f));
  const LoadoutState saved = loadout.SaveState();
  CHECK(saved.slots[static_cast<size_t>(WeaponSlot::kRifle)].present);
  CHECK(saved.slots[static_cast<size_t>(WeaponSlot::kRifle)]
            .scope_magnification == 4.0f);
  REQUIRE(loadout.SetScopeZero(100.0f));
  CHECK(loadout.RestoreState(saved));
  CHECK(loadout.active_item()->scope_zero == 300.0f);
}

TEST_CASE("restoring onto other slots reports the mismatch") {
  Loadout both = MakeLoadout();
  (void)both.Update(kTrigger, kTick);  // one rifle shot
  const LoadoutState saved = both.SaveState();

  SUBCASE("a slot of the state is empty here") {
    const std::array<WeaponItem, 1> rifle_only{Rifle()};
    Loadout loadout(rifle_only);
    CHECK_FALSE(loadout.RestoreState(saved));
    // The shared rifle slot was still restored.
    CHECK(loadout.firearm(WeaponSlot::kRifle)->SaveState() ==
          saved.slots[static_cast<size_t>(WeaponSlot::kRifle)].firearm);
    CHECK(loadout.active() == WeaponSlot::kRifle);
  }
  SUBCASE("a slot here is empty in the state") {
    const std::array<WeaponItem, 1> rifle_only{Rifle()};
    const LoadoutState rifle_state = Loadout(rifle_only).SaveState();
    Loadout loadout = MakeLoadout();
    CHECK_FALSE(loadout.RestoreState(rifle_state));
    CHECK(loadout.item(WeaponSlot::kPistol) != nullptr);
  }
  SUBCASE("the switch state names an empty slot: switch left alone") {
    const std::array<WeaponItem, 1> pistol_only{Pistol()};
    Loadout loadout(pistol_only);
    CHECK_FALSE(loadout.RestoreState(saved));
    CHECK(loadout.active() == WeaponSlot::kPistol);
    CHECK(loadout.selected() == WeaponSlot::kPistol);
    // Still usable.
    CHECK(loadout.Update(kTrigger, kTick).firearm.shots == 1);
  }
}

TEST_CASE("loadout states compare within the tolerance") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update({.select = WeaponSlot::kPistol}, kTick);
  const LoadoutState a = loadout.SaveState();
  LoadoutState b = a;
  CHECK(NearlyEqual(a, b));
  b.raise += 0.5f * kStateTimeTolerance;
  CHECK_FALSE(a == b);
  CHECK(NearlyEqual(a, b));
  b.raise += kStateTimeTolerance;
  CHECK_FALSE(NearlyEqual(a, b));
  b = a;
  b.slots[0].firearm.cycle_remaining += 0.5f * kStateTimeTolerance;
  CHECK(NearlyEqual(a, b));
  b.slots[0].firearm.chambered = !b.slots[0].firearm.chambered;
  CHECK_FALSE(NearlyEqual(a, b));
  b = a;
  b.selected = WeaponSlot::kRifle;
  CHECK_FALSE(NearlyEqual(a, b));
}

}  // namespace
}  // namespace blue::weapon
