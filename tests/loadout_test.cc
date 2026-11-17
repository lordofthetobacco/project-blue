#include "weapon/loadout.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>

#include "weapon/firearm.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;
// Long enough after a pull for either weapon's action to cycle.
constexpr float kRelease = 0.2f;

Loadout MakeLoadout() { return Loadout(Glock17Spec(), M16Spec()); }

LoadoutInput Select(WeaponSlot slot) { return {.select = slot}; }
LoadoutInput Trigger() { return {.firearm = {.trigger_held = true}}; }
LoadoutInput Reload() { return {.firearm = {.reload_pressed = true}}; }

// Runs idle frames until no switch is in progress (at most 5 s); returns
// how many frames reported weapon_changed.
int FinishSwitch(Loadout& loadout, LoadoutInput input = {}) {
  int changes = 0;
  for (int i = 0; i < 300 && loadout.switching(); ++i) {
    changes += loadout.Update(input, kFrame).weapon_changed ? 1 : 0;
  }
  return changes;
}

TEST_CASE("the loadout starts with the rifle ready") {
  const Loadout loadout = MakeLoadout();
  CHECK(loadout.active() == WeaponSlot::kRifle);
  CHECK(loadout.selected() == WeaponSlot::kRifle);
  CHECK_FALSE(loadout.switching());
  CHECK(loadout.raise_fraction() == 1.0f);
  CHECK(std::string(loadout.active_firearm().spec().display_name) == "M16A2");
  CHECK(std::string(loadout.firearm(WeaponSlot::kPistol).spec().display_name) ==
        "Glock 17");
}

TEST_CASE("selecting the weapon in hand does nothing") {
  Loadout loadout = MakeLoadout();
  const LoadoutEvents events =
      loadout.Update(Select(WeaponSlot::kRifle), kFrame);
  CHECK_FALSE(events.switch_started);
  CHECK_FALSE(events.weapon_changed);
  CHECK_FALSE(loadout.switching());
  CHECK(loadout.raise_fraction() == 1.0f);
}

TEST_CASE("a switch lowers, swaps once and raises over the tuned times") {
  Loadout loadout = MakeLoadout();
  const LoadoutTuning& tuning = loadout.tuning();
  LoadoutEvents events = loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  CHECK(events.switch_started);
  CHECK(loadout.switching());
  CHECK(loadout.selected() == WeaponSlot::kPistol);
  CHECK(loadout.active() == WeaponSlot::kRifle);
  float elapsed = kFrame;
  float previous = loadout.raise_fraction();
  CHECK(previous < 1.0f);
  float swap_time = -1.0f;
  float largest_step = 0.0f;
  while (loadout.switching() && elapsed < 5.0f) {
    events = loadout.Update({}, kFrame);
    elapsed += kFrame;
    const float fraction = loadout.raise_fraction();
    largest_step = std::max(largest_step, std::abs(fraction - previous));
    if (events.weapon_changed) {
      CHECK(swap_time < 0.0f);
      swap_time = elapsed;
      CHECK(fraction < 0.01f);
      CHECK(loadout.active() == WeaponSlot::kPistol);
    } else if (swap_time < 0.0f) {
      CHECK(fraction <= previous);  // lowering
    } else {
      CHECK(fraction >= previous);  // raising
    }
    previous = fraction;
  }
  CHECK(swap_time == doctest::Approx(tuning.holster_seconds).epsilon(0.05));
  CHECK(elapsed == doctest::Approx(tuning.holster_seconds + tuning.draw_seconds)
                       .epsilon(0.05));
  CHECK(loadout.raise_fraction() == 1.0f);
  CHECK(loadout.active() == WeaponSlot::kPistol);
  // Continuous: no frame jumps more than an eased step can.
  CHECK(largest_step < 0.1f);
}

TEST_CASE("one huge frame still reports the swap and finishes the switch") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), 0.0f);
  const LoadoutEvents events = loadout.Update({}, 10.0f);
  CHECK(events.weapon_changed);
  CHECK(loadout.active() == WeaponSlot::kPistol);
  CHECK_FALSE(loadout.switching());
}

TEST_CASE("the switch is frame-rate independent") {
  Loadout coarse = MakeLoadout();
  Loadout fine = MakeLoadout();
  (void)coarse.Update(Select(WeaponSlot::kPistol), 0.0f);
  (void)fine.Update(Select(WeaponSlot::kPistol), 0.0f);
  for (int i = 0; i < 30; ++i) {
    (void)coarse.Update({}, 1.0f / 30.0f);
    for (int step = 0; step < 8; ++step) {
      (void)fine.Update({}, 1.0f / 240.0f);
    }
    CHECK(coarse.active() == fine.active());
    CHECK(coarse.raise_fraction() ==
          doctest::Approx(fine.raise_fraction()).epsilon(1.0e-4));
  }
}

TEST_CASE("re-selecting mid-holster turns back from the current height") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), 0.1f);
  const float lowered = loadout.raise_fraction();
  REQUIRE(lowered < 1.0f);
  REQUIRE(lowered > 0.0f);
  const LoadoutEvents events = loadout.Update(Select(WeaponSlot::kRifle), 0.0f);
  CHECK(events.switch_started);
  CHECK(loadout.raise_fraction() == lowered);
  CHECK(loadout.selected() == WeaponSlot::kRifle);
  (void)loadout.Update({}, kFrame);
  CHECK(loadout.raise_fraction() > lowered);
  CHECK(FinishSwitch(loadout) == 0);
  CHECK(loadout.active() == WeaponSlot::kRifle);
}

TEST_CASE("selecting during the raise lowers again from the current height") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), 0.0f);
  (void)loadout.Update({}, loadout.tuning().holster_seconds + 0.1f);
  REQUIRE(loadout.active() == WeaponSlot::kPistol);
  const float raised = loadout.raise_fraction();
  REQUIRE(raised > 0.0f);
  REQUIRE(raised < 1.0f);
  CHECK(loadout.Update(Select(WeaponSlot::kRifle), 0.0f).switch_started);
  CHECK(loadout.raise_fraction() == raised);
  (void)loadout.Update({}, kFrame);
  CHECK(loadout.raise_fraction() < raised);
  CHECK(FinishSwitch(loadout) == 1);
  CHECK(loadout.active() == WeaponSlot::kRifle);
}

TEST_CASE("no firing or reloading until the weapon is up") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  int shots = 0;
  bool dry = false;
  bool reload = false;
  while (loadout.switching()) {
    LoadoutInput input = Trigger();
    input.firearm.reload_pressed = true;
    const LoadoutEvents events = loadout.Update(input, kFrame);
    shots += events.firearm.shots;
    dry = dry || events.firearm.dry_fire;
    reload = reload || events.firearm.reload_started;
  }
  CHECK(shots == 0);
  CHECK_FALSE(dry);
  CHECK_FALSE(reload);
  // The trigger held through the switch must be released first.
  CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 0);
  CHECK(loadout.Update({}, kFrame).firearm.shots == 0);
  CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 1);
}

TEST_CASE("an auto rifle also needs a fresh pull after the switch") {
  FirearmSpec rifle = M16Spec();
  rifle.fire_mode = FireMode::kAuto;
  Loadout loadout(Glock17Spec(), rifle);
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  FinishSwitch(loadout, Trigger());
  (void)loadout.Update(Select(WeaponSlot::kRifle), kFrame);
  FinishSwitch(loadout, Trigger());
  REQUIRE(loadout.active() == WeaponSlot::kRifle);
  CHECK(loadout.Update(Trigger(), 0.5f).firearm.shots == 0);
  (void)loadout.Update({}, kFrame);
  CHECK(loadout.Update(Trigger(), 0.5f).firearm.shots > 1);
}

TEST_CASE("only the weapon in hand fires and returns events") {
  Loadout loadout = MakeLoadout();
  CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 1);
  (void)loadout.Update({}, kRelease);
  const Firearm rifle_before = loadout.firearm(WeaponSlot::kRifle);
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  FinishSwitch(loadout);
  for (int i = 0; i < 3; ++i) {
    CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 1);
    (void)loadout.Update({}, kRelease);
  }
  const Firearm& rifle = loadout.firearm(WeaponSlot::kRifle);
  CHECK(rifle.rounds_in_magazine() == rifle_before.rounds_in_magazine());
  CHECK(rifle.round_chambered() == rifle_before.round_chambered());
  CHECK(loadout.firearm(WeaponSlot::kPistol).rounds_loaded() == 18 - 3);
  CHECK(std::string(loadout.active_firearm().spec().display_name) ==
        "Glock 17");
}

TEST_CASE("each weapon keeps its ammunition while holstered") {
  Loadout loadout = MakeLoadout();
  for (int i = 0; i < 5; ++i) {
    (void)loadout.Update(Trigger(), kFrame);
    (void)loadout.Update({}, kRelease);
  }
  REQUIRE(loadout.firearm(WeaponSlot::kRifle).rounds_loaded() == 31 - 5);
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  FinishSwitch(loadout);
  (void)loadout.Update(Trigger(), kFrame);
  (void)loadout.Update({}, kRelease);
  (void)loadout.Update(Select(WeaponSlot::kRifle), kFrame);
  FinishSwitch(loadout);
  CHECK(loadout.firearm(WeaponSlot::kRifle).rounds_loaded() == 26);
  CHECK(loadout.firearm(WeaponSlot::kPistol).rounds_loaded() == 17);
}

TEST_CASE("starting a switch cancels the reload in progress") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Trigger(), kFrame);
  (void)loadout.Update({}, kRelease);
  REQUIRE(loadout.Update(Reload(), kFrame).firearm.reload_started);
  (void)loadout.Update({}, 0.2f);
  REQUIRE(loadout.active_firearm().reloading());
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  const Firearm& rifle = loadout.firearm(WeaponSlot::kRifle);
  CHECK_FALSE(rifle.reloading());
  // Aborted before the magazine came out: unchanged, all spares kept.
  CHECK(rifle.magazine_inserted());
  CHECK(rifle.rounds_in_magazine() == 29);
  CHECK(rifle.spare_magazines().size() == 4);
  // Holstered, it stays that way.
  FinishSwitch(loadout);
  CHECK_FALSE(loadout.firearm(WeaponSlot::kRifle).reloading());
}

TEST_CASE("the fire selector is blocked while switching") {
  FirearmSpec rifle = M16Spec();
  Loadout loadout(Glock17Spec(), rifle);
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  LoadoutInput toggle;
  toggle.firearm.fire_mode_pressed = true;
  toggle.select = WeaponSlot::kRifle;
  CHECK_FALSE(loadout.Update(toggle, kFrame).firearm.fire_mode_changed);
  FinishSwitch(loadout);
  toggle.select.reset();
  CHECK(loadout.Update(toggle, kFrame).firearm.fire_mode_changed);
}

}  // namespace
}  // namespace blue::weapon
