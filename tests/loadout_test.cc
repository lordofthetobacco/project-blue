#include "weapon/loadout.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "tests/part_catalog_samples.h"
#include "weapon/firearm.h"
#include "weapon/parts/part_catalog.h"
#include "weapon/parts/weapon_build.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;
// Long enough after a pull for either weapon's action to cycle.
constexpr float kRelease = 0.2f;

// Definition ids the loadout carries around without looking at them.
constexpr WeaponDefId kPistolDef{3};
constexpr WeaponDefId kRifleDef{5};
constexpr WeaponDefId kOtherPistolDef{8};

// A fresh pistol and `rifle`, the rifle in hand.
Loadout MakeLoadout(const FirearmSpec& rifle = M16Spec()) {
  const std::array<WeaponItem, 2> weapons{
      WeaponItem{.def = kPistolDef, .firearm = Firearm(Glock17Spec())},
      WeaponItem{.def = kRifleDef, .firearm = Firearm(rifle)}};
  return Loadout(weapons);
}

WeaponItem FreshPistol(WeaponDefId def = kPistolDef) {
  return {.def = def, .firearm = Firearm(Glock17Spec())};
}

LoadoutInput Select(WeaponSlot slot) { return {.select = slot}; }
LoadoutInput Trigger() { return {.firearm = {.trigger_held = true}}; }
LoadoutInput Reload() { return {.firearm = {.reload_pressed = true}}; }

// Whether `a` and `b` are in the same observable state.
bool SameState(const Firearm& a, const Firearm& b) {
  const std::vector<int> spares_a(a.spare_magazines().begin(),
                                  a.spare_magazines().end());
  const std::vector<int> spares_b(b.spare_magazines().begin(),
                                  b.spare_magazines().end());
  return a.spec().display_name == b.spec().display_name &&
         a.rounds_in_magazine() == b.rounds_in_magazine() &&
         a.magazine_inserted() == b.magazine_inserted() &&
         a.round_chambered() == b.round_chambered() &&
         a.bolt_locked_back() == b.bolt_locked_back() &&
         a.reloading() == b.reloading() && a.fire_mode() == b.fire_mode() &&
         spares_a == spares_b;
}

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
  CHECK(std::string(loadout.active_firearm()->spec().display_name) == "M16A2");
  CHECK(
      std::string(loadout.firearm(WeaponSlot::kPistol)->spec().display_name) ==
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
  Loadout loadout = MakeLoadout(rifle);
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
  const Firearm rifle_before = *loadout.firearm(WeaponSlot::kRifle);
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  FinishSwitch(loadout);
  for (int i = 0; i < 3; ++i) {
    CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 1);
    (void)loadout.Update({}, kRelease);
  }
  const Firearm& rifle = *loadout.firearm(WeaponSlot::kRifle);
  CHECK(rifle.rounds_in_magazine() == rifle_before.rounds_in_magazine());
  CHECK(rifle.round_chambered() == rifle_before.round_chambered());
  CHECK(loadout.firearm(WeaponSlot::kPistol)->rounds_loaded() == 18 - 3);
  CHECK(std::string(loadout.active_firearm()->spec().display_name) ==
        "Glock 17");
}

TEST_CASE("each weapon keeps its ammunition while holstered") {
  Loadout loadout = MakeLoadout();
  for (int i = 0; i < 5; ++i) {
    (void)loadout.Update(Trigger(), kFrame);
    (void)loadout.Update({}, kRelease);
  }
  REQUIRE(loadout.firearm(WeaponSlot::kRifle)->rounds_loaded() == 31 - 5);
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  FinishSwitch(loadout);
  (void)loadout.Update(Trigger(), kFrame);
  (void)loadout.Update({}, kRelease);
  (void)loadout.Update(Select(WeaponSlot::kRifle), kFrame);
  FinishSwitch(loadout);
  CHECK(loadout.firearm(WeaponSlot::kRifle)->rounds_loaded() == 26);
  CHECK(loadout.firearm(WeaponSlot::kPistol)->rounds_loaded() == 17);
}

TEST_CASE("starting a switch cancels the reload in progress") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Trigger(), kFrame);
  (void)loadout.Update({}, kRelease);
  REQUIRE(loadout.Update(Reload(), kFrame).firearm.reload_started);
  (void)loadout.Update({}, 0.2f);
  REQUIRE(loadout.active_firearm()->reloading());
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  const Firearm& rifle = *loadout.firearm(WeaponSlot::kRifle);
  CHECK_FALSE(rifle.reloading());
  // Aborted before the magazine came out: unchanged, all spares kept.
  CHECK(rifle.magazine_inserted());
  CHECK(rifle.rounds_in_magazine() == 29);
  CHECK(rifle.spare_magazines().size() == 4);
  // Holstered, it stays that way.
  FinishSwitch(loadout);
  CHECK_FALSE(loadout.firearm(WeaponSlot::kRifle)->reloading());
}

TEST_CASE("the fire selector is blocked while switching") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  LoadoutInput toggle;
  toggle.firearm.fire_mode_pressed = true;
  toggle.select = WeaponSlot::kRifle;
  CHECK_FALSE(loadout.Update(toggle, kFrame).firearm.fire_mode_changed);
  FinishSwitch(loadout);
  toggle.select.reset();
  CHECK(loadout.Update(toggle, kFrame).firearm.fire_mode_changed);
}

// Fires `shots` single shots from the weapon in hand.
void FireShots(Loadout& loadout, int shots) {
  for (int i = 0; i < shots; ++i) {
    REQUIRE(loadout.Update(Trigger(), kFrame).firearm.shots == 1);
    (void)loadout.Update({}, kRelease);
  }
}

TEST_CASE("weapon kinds map to their slots") {
  CHECK(SlotFor(WeaponKind::kPistol) == WeaponSlot::kPistol);
  CHECK(SlotFor(WeaponKind::kRifle) == WeaponSlot::kRifle);
  CHECK(Glock17Spec().kind == WeaponKind::kPistol);
  CHECK(M16Spec().kind == WeaponKind::kRifle);
}

TEST_CASE("a dropped weapon leaves at once and the other one comes up") {
  Loadout loadout = MakeLoadout();
  FireShots(loadout, 3);
  LoadoutInput toggle;
  toggle.firearm.fire_mode_pressed = true;
  REQUIRE(loadout.Update(toggle, kFrame).firearm.fire_mode_changed);
  const Firearm before = *loadout.active_firearm();

  const std::optional<WeaponItem> dropped = loadout.Drop();
  REQUIRE(dropped.has_value());
  CHECK(dropped->def == kRifleDef);
  CHECK(SameState(dropped->firearm, before));
  CHECK(dropped->firearm.fire_mode() == FireMode::kAuto);
  CHECK(loadout.firearm(WeaponSlot::kRifle) == nullptr);
  // The pistol is drawn from the bottom with the normal timing.
  CHECK(loadout.active() == WeaponSlot::kPistol);
  CHECK(loadout.selected() == WeaponSlot::kPistol);
  CHECK(loadout.raise_fraction() == 0.0f);
  CHECK(loadout.switching());
  float elapsed = 0.0f;
  while (loadout.switching() && elapsed < 5.0f) {
    CHECK_FALSE(loadout.Update({}, kFrame).weapon_changed);
    elapsed += kFrame;
  }
  CHECK(elapsed ==
        doctest::Approx(loadout.tuning().draw_seconds).epsilon(0.05));
  CHECK(loadout.active_firearm()->spec().kind == WeaponKind::kPistol);
}

TEST_CASE("drop then pick up gives back the identical firearm") {
  Loadout loadout = MakeLoadout();
  FireShots(loadout, 4);
  const Firearm before = *loadout.active_firearm();
  std::optional<WeaponItem> dropped = loadout.Drop();
  REQUIRE(dropped.has_value());
  FinishSwitch(loadout);
  REQUIRE(loadout.active() == WeaponSlot::kPistol);

  const PickUpResult result = loadout.PickUp(std::move(*dropped));
  CHECK_FALSE(result.replaced.has_value());
  // The pistol in hand is holstered first, as in a normal switch.
  CHECK_FALSE(result.weapon_changed);
  CHECK(loadout.active() == WeaponSlot::kPistol);
  CHECK(loadout.selected() == WeaponSlot::kRifle);
  CHECK(FinishSwitch(loadout) == 1);
  CHECK(loadout.active() == WeaponSlot::kRifle);
  CHECK(SameState(*loadout.active_firearm(), before));
  CHECK(loadout.active_item()->def == kRifleDef);
  CHECK(loadout.active_firearm()->rounds_loaded() == 31 - 4);
}

TEST_CASE("the scope magnification and zero go with the weapon") {
  Loadout loadout = MakeLoadout();
  REQUIRE(loadout.SetScopeMagnification(6.5f));
  REQUIRE(loadout.SetScopeZero(250.0f));
  CHECK(loadout.active_item()->scope_magnification == 6.5f);

  // Kept while holstered and drawn again.
  loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  (void)FinishSwitch(loadout);
  CHECK(loadout.item(WeaponSlot::kRifle)->scope_magnification == 6.5f);
  loadout.Update(Select(WeaponSlot::kRifle), kFrame);
  (void)FinishSwitch(loadout);
  REQUIRE(loadout.active() == WeaponSlot::kRifle);
  CHECK(loadout.active_item()->scope_magnification == 6.5f);

  // Dropped and picked up again: the same magnification.
  std::optional<WeaponItem> dropped = loadout.Drop();
  REQUIRE(dropped.has_value());
  CHECK(dropped->scope_magnification == 6.5f);
  const PickUpResult result = loadout.PickUp(std::move(*dropped));
  CHECK_FALSE(result.replaced.has_value());
  CHECK(loadout.item(WeaponSlot::kRifle)->scope_magnification == 6.5f);
  CHECK(loadout.item(WeaponSlot::kRifle)->scope_zero == 250.0f);

  // The other weapons are untouched; empty hands have nothing to set.
  CHECK(loadout.item(WeaponSlot::kPistol)->scope_magnification == 0.0f);
  Loadout empty;
  CHECK_FALSE(empty.SetScopeMagnification(4.0f));
  CHECK_FALSE(empty.SetScopeZero(200.0f));
}

TEST_CASE("dropping the last weapon leaves the hands empty") {
  Loadout loadout = MakeLoadout();
  REQUIRE(loadout.Drop().has_value());
  FinishSwitch(loadout);
  const std::optional<WeaponItem> pistol = loadout.Drop();
  REQUIRE(pistol.has_value());
  CHECK(pistol->firearm.spec().kind == WeaponKind::kPistol);
  CHECK(loadout.hands_empty());
  CHECK_FALSE(loadout.active().has_value());
  CHECK_FALSE(loadout.selected().has_value());
  CHECK(loadout.active_firearm() == nullptr);
  CHECK_FALSE(loadout.switching());
  CHECK(loadout.raise_fraction() == 0.0f);

  // Nothing to fire, reload or throw, and empty slots cannot be selected.
  LoadoutInput input = Trigger();
  input.firearm.reload_pressed = true;
  input.select = WeaponSlot::kPistol;
  const LoadoutEvents events = loadout.Update(input, kFrame);
  CHECK(events.firearm.shots == 0);
  CHECK_FALSE(events.firearm.dry_fire);
  CHECK_FALSE(events.firearm.reload_started);
  CHECK_FALSE(events.switch_started);
  CHECK_FALSE(events.weapon_changed);
  CHECK_FALSE(loadout.Drop().has_value());
  CHECK(loadout.hands_empty());
}

TEST_CASE("a weapon picked up with empty hands comes up from the bottom") {
  Loadout loadout;
  REQUIRE(loadout.hands_empty());
  const PickUpResult result = loadout.PickUp(FreshPistol());
  CHECK(result.weapon_changed);
  CHECK_FALSE(result.replaced.has_value());
  CHECK(loadout.active() == WeaponSlot::kPistol);
  CHECK(loadout.raise_fraction() == 0.0f);
  CHECK(loadout.switching());
  // A trigger held while it comes up must be released first.
  CHECK(FinishSwitch(loadout, Trigger()) == 0);
  CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 0);
  (void)loadout.Update({}, kFrame);
  CHECK(loadout.Update(Trigger(), kFrame).firearm.shots == 1);
}

TEST_CASE("picking up the kind in hand swaps it straight away") {
  Loadout loadout = MakeLoadout();
  FireShots(loadout, 2);
  const Firearm held = *loadout.active_firearm();
  // A second rifle, a few rounds down.
  Loadout other = MakeLoadout();
  FireShots(other, 7);
  const Firearm found = *other.active_firearm();

  PickUpResult result = loadout.PickUp({.def = kRifleDef, .firearm = found});
  REQUIRE(result.replaced.has_value());
  CHECK(SameState(result.replaced->firearm, held));
  CHECK(result.weapon_changed);
  CHECK(loadout.active() == WeaponSlot::kRifle);
  CHECK(loadout.raise_fraction() == 0.0f);
  CHECK(SameState(*loadout.active_firearm(), found));
  // The pistol stays where it was.
  CHECK(loadout.firearm(WeaponSlot::kPistol)->rounds_loaded() == 18);
  CHECK(FinishSwitch(loadout) == 0);
  CHECK(loadout.active_firearm()->rounds_loaded() == 31 - 7);
}

TEST_CASE("picking up the holstered kind swaps the holstered weapon") {
  Loadout loadout = MakeLoadout();
  const Firearm pistol_before = *loadout.firearm(WeaponSlot::kPistol);
  PickUpResult result = loadout.PickUp(FreshPistol());
  REQUIRE(result.replaced.has_value());
  CHECK(SameState(result.replaced->firearm, pistol_before));
  CHECK_FALSE(result.weapon_changed);
  CHECK(loadout.selected() == WeaponSlot::kPistol);
  CHECK(FinishSwitch(loadout) == 1);
  CHECK(loadout.active() == WeaponSlot::kPistol);
}

TEST_CASE("no drop while the weapon in hand is lowered; allowed while rising") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), 0.1f);
  REQUIRE(loadout.active() == WeaponSlot::kRifle);
  REQUIRE(loadout.selected() == WeaponSlot::kPistol);
  const Loadout lowering = loadout;
  CHECK_FALSE(loadout.Drop().has_value());
  // Refused without a trace.
  CHECK(loadout.firearm(WeaponSlot::kRifle) != nullptr);
  CHECK(loadout.firearm(WeaponSlot::kPistol) != nullptr);
  CHECK(loadout.raise_fraction() == lowering.raise_fraction());
  CHECK(loadout.selected() == WeaponSlot::kPistol);

  (void)loadout.Update({}, loadout.tuning().holster_seconds);
  REQUIRE(loadout.active() == WeaponSlot::kPistol);
  REQUIRE(loadout.switching());
  const std::optional<WeaponItem> pistol = loadout.Drop();
  REQUIRE(pistol.has_value());
  CHECK(pistol->firearm.spec().kind == WeaponKind::kPistol);
  CHECK(loadout.active() == WeaponSlot::kRifle);
  CHECK(loadout.raise_fraction() == 0.0f);
  CHECK(loadout.firearm(WeaponSlot::kPistol) == nullptr);
  CHECK(FinishSwitch(loadout) == 0);
}

TEST_CASE("dropping cancels the reload in progress") {
  Loadout loadout = MakeLoadout();
  FireShots(loadout, 1);
  REQUIRE(loadout.Update(Reload(), kFrame).firearm.reload_started);
  (void)loadout.Update({}, 0.2f);
  REQUIRE(loadout.active_firearm()->reloading());
  const std::optional<WeaponItem> rifle = loadout.Drop();
  REQUIRE(rifle.has_value());
  CHECK_FALSE(rifle->firearm.reloading());
  CHECK(rifle->firearm.magazine_inserted());
  CHECK(rifle->firearm.rounds_in_magazine() == 29);
  CHECK(rifle->firearm.spare_magazines().size() == 4);
}

TEST_CASE("two pistols of different definitions swap in the pistol slot") {
  Loadout loadout = MakeLoadout();
  FireShots(loadout, 1);  // The rifle, in hand.
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  FinishSwitch(loadout);
  REQUIRE(loadout.active_item()->def == kPistolDef);
  FireShots(loadout, 2);
  const Firearm old_pistol = *loadout.active_firearm();

  // The other pistol replaces the one in hand at once; the old one comes
  // back with its own definition and state, the rifle is untouched.
  PickUpResult result = loadout.PickUp(FreshPistol(kOtherPistolDef));
  REQUIRE(result.replaced.has_value());
  CHECK(result.replaced->def == kPistolDef);
  CHECK(SameState(result.replaced->firearm, old_pistol));
  CHECK(result.weapon_changed);
  CHECK(loadout.active_item()->def == kOtherPistolDef);
  CHECK(loadout.active_firearm()->rounds_loaded() == 18);
  CHECK(loadout.item(WeaponSlot::kRifle)->def == kRifleDef);

  // Swapping back: the first pistol is picked up again as it was.
  FinishSwitch(loadout);
  result = loadout.PickUp(std::move(*result.replaced));
  REQUIRE(result.replaced.has_value());
  CHECK(result.replaced->def == kOtherPistolDef);
  CHECK(loadout.active_item()->def == kPistolDef);
  CHECK(SameState(*loadout.active_firearm(), old_pistol));
}

TEST_CASE("the weapon list constructor fills slots, last one in hand") {
  const Loadout empty(std::span<const WeaponItem>{});
  CHECK(empty.hands_empty());
  const std::array<WeaponItem, 1> pistol_only{FreshPistol()};
  const Loadout pistol(pistol_only);
  CHECK(pistol.active() == WeaponSlot::kPistol);
  CHECK_FALSE(pistol.switching());
  CHECK(pistol.item(WeaponSlot::kRifle) == nullptr);
  // A later weapon of the same kind replaces an earlier one.
  const std::array<WeaponItem, 2> two_pistols{FreshPistol(),
                                              FreshPistol(kOtherPistolDef)};
  const Loadout replaced(two_pistols);
  CHECK(replaced.active_item()->def == kOtherPistolDef);
}

TEST_CASE("a loadout is copied with its weapons") {
  Loadout loadout = MakeLoadout();
  const Loadout copy = loadout;
  REQUIRE(loadout.Drop().has_value());
  CHECK(copy.firearm(WeaponSlot::kRifle) != nullptr);
  CHECK(copy.active() == WeaponSlot::kRifle);
}

// Seconds from selecting `slot` until the switch is over.
float SwitchSeconds(Loadout& loadout, WeaponSlot slot) {
  (void)loadout.Update(Select(slot), kFrame);
  float elapsed = kFrame;
  while (loadout.switching() && elapsed < 10.0f) {
    (void)loadout.Update({}, kFrame);
    elapsed += kFrame;
  }
  return elapsed;
}

TEST_CASE("a weapon's switch time scale stretches its holster and draw") {
  Loadout normal = MakeLoadout();
  const LoadoutTuning& tuning = normal.tuning();
  WeaponItem slow_pistol = FreshPistol();
  slow_pistol.switch_time_scale = 2.0f;
  const std::array<WeaponItem, 2> weapons{
      slow_pistol, WeaponItem{.def = kRifleDef, .firearm = Firearm(M16Spec())}};
  Loadout slow(weapons);
  CHECK(SwitchSeconds(normal, WeaponSlot::kPistol) ==
        doctest::Approx(tuning.holster_seconds + tuning.draw_seconds)
            .epsilon(0.05));
  // The rifle goes down as usual, the pistol comes up twice as slowly.
  CHECK(SwitchSeconds(slow, WeaponSlot::kPistol) ==
        doctest::Approx(tuning.holster_seconds + 2.0f * tuning.draw_seconds)
            .epsilon(0.05));
  // Back: the pistol goes down twice as slowly.
  CHECK(SwitchSeconds(slow, WeaponSlot::kRifle) ==
        doctest::Approx(2.0f * tuning.holster_seconds + tuning.draw_seconds)
            .epsilon(0.05));
}

TEST_CASE("ReplaceActive changes the weapon in hand, not the switch") {
  Loadout loadout = MakeLoadout();
  (void)loadout.Update(Select(WeaponSlot::kPistol), kFrame);
  (void)loadout.Update({}, kFrame);
  const float raise = loadout.raise_fraction();
  WeaponItem rifle = *loadout.active_item();
  FirearmSpec spec = M16Spec();
  spec.magazine_capacity = 20;
  rifle.firearm.Refit(spec);
  rifle.def = WeaponDefId{9};
  CHECK(loadout.ReplaceActive(rifle));
  CHECK(loadout.active_item()->def == WeaponDefId{9});
  CHECK(loadout.active_firearm()->rounds_in_magazine() == 20);
  CHECK(loadout.raise_fraction() == raise);
  CHECK(loadout.selected() == WeaponSlot::kPistol);
  // A weapon of the other slot's kind is refused.
  CHECK_FALSE(loadout.ReplaceActive(FreshPistol(kOtherPistolDef)));
  CHECK(loadout.active_item()->def == WeaponDefId{9});

  Loadout empty;
  CHECK_FALSE(empty.ReplaceActive(FreshPistol()));
  CHECK(empty.hands_empty());
}

TEST_CASE("a weapon's parts go with it through a drop and a pick-up") {
  const parts::PartCatalog catalog = parts::testing::GlockCatalog();
  const parts::PresetDef* preset = catalog.FindPreset("glock17");
  REQUIRE(preset != nullptr);
  const auto build = parts::MakeBuild(catalog, *preset);
  REQUIRE(build.ok());
  WeaponItem pistol = FreshPistol();
  pistol.build = build.value();
  pistol.switch_time_scale = 1.3f;
  const std::array<WeaponItem, 1> weapons{pistol};
  Loadout loadout(weapons);
  std::optional<WeaponItem> dropped = loadout.Drop();
  REQUIRE(dropped.has_value());
  CHECK(dropped->build == build.value());
  CHECK(loadout.hands_empty());
  (void)loadout.PickUp(std::move(*dropped));
  REQUIRE(loadout.active_item() != nullptr);
  CHECK(loadout.active_item()->build == build.value());
  CHECK(loadout.active_item()->switch_time_scale == 1.3f);
}

}  // namespace
}  // namespace blue::weapon
