#include "weapon/firearm.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
#include <span>

#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;

FirearmInput Trigger() { return {.trigger_held = true}; }
FirearmInput Idle() { return {}; }
FirearmInput Reload() { return {.reload_pressed = true}; }

FirearmSpec AutoSpec() {
  FirearmSpec spec;
  spec.fire_mode = FireMode::kAuto;
  return spec;
}

// Pulls and releases the trigger once; returns the shots fired.
int PullOnce(Firearm& firearm) {
  const int shots = firearm.Update(Trigger(), kFrame).shots;
  (void)firearm.Update(Idle(), 1.0f);  // release and let the bolt cycle
  return shots;
}

// Runs frames of `dt` until `seconds` have passed; returns the events summed.
FirearmEvents RunFor(Firearm& firearm, const FirearmInput& input, float dt,
                     float seconds) {
  FirearmEvents total;
  const int frames = static_cast<int>(seconds / dt + 0.5f);
  for (int i = 0; i < frames; ++i) {
    const FirearmEvents events = firearm.Update(input, dt);
    total.shots += events.shots;
    total.dry_fire = total.dry_fire || events.dry_fire;
    total.reload_started = total.reload_started || events.reload_started;
    total.reload_finished = total.reload_finished || events.reload_finished;
  }
  return total;
}

int TotalSpareRounds(const Firearm& firearm) {
  const std::span<const int> spares = firearm.spare_magazines();
  return std::accumulate(spares.begin(), spares.end(), 0);
}

// Fires semi shots until the chamber is empty; returns the shots fired.
int EmptyTheRifle(Firearm& firearm) {
  int shots = 0;
  while (firearm.round_chambered()) {
    shots += PullOnce(firearm);
  }
  return shots;
}

TEST_CASE("a fresh rifle is loaded: full magazine plus one chambered") {
  const Firearm firearm;
  CHECK(firearm.rounds_in_magazine() == 30);
  CHECK(firearm.round_chambered());
  CHECK(firearm.rounds_loaded() == 31);
  CHECK_FALSE(firearm.bolt_locked_back());
  CHECK_FALSE(firearm.reloading());
  CHECK(firearm.reload_fraction() == 0.0f);
  CHECK(firearm.fire_mode() == FireMode::kSemi);
  REQUIRE(firearm.spare_magazines().size() == 4);
  CHECK(TotalSpareRounds(firearm) == 120);
}

TEST_CASE("a fresh rifle fires 31 times, then the bolt locks back") {
  Firearm firearm;
  CHECK(EmptyTheRifle(firearm) == 31);
  CHECK(firearm.rounds_in_magazine() == 0);
  CHECK_FALSE(firearm.round_chambered());
  CHECK(firearm.bolt_locked_back());
}

TEST_CASE("each shot strips the next round into the chamber") {
  Firearm firearm;
  CHECK(PullOnce(firearm) == 1);
  CHECK(firearm.rounds_in_magazine() == 29);
  CHECK(firearm.round_chambered());
  CHECK_FALSE(firearm.bolt_locked_back());
}

TEST_CASE("semi fires once per trigger pull") {
  Firearm firearm;
  const FirearmEvents first = firearm.Update(Trigger(), kFrame);
  CHECK(first.shots == 1);
  const FirearmEvents held = RunFor(firearm, Trigger(), kFrame, 1.0f);
  CHECK(held.shots == 0);
  (void)firearm.Update(Idle(), kFrame);
  CHECK(firearm.Update(Trigger(), kFrame).shots == 1);
}

TEST_CASE("semi cannot fire faster than the bolt cycles") {
  Firearm firearm;
  CHECK(firearm.Update(Trigger(), 0.01f).shots == 1);
  (void)firearm.Update(Idle(), 0.01f);
  // 75 ms cycle: 55 ms remain, so this pull fires once the bolt is back.
  const FirearmEvents early = firearm.Update(Trigger(), 0.05f);
  CHECK(early.shots == 0);
  const FirearmEvents ready = firearm.Update(Trigger(), 0.01f);
  CHECK(ready.shots == 1);
  CHECK(ready.shot_age[0] == doctest::Approx(0.005f));
}

TEST_CASE("dry fire clicks once per pull on an empty chamber") {
  Firearm firearm;
  EmptyTheRifle(firearm);
  const FirearmEvents click = firearm.Update(Trigger(), kFrame);
  CHECK(click.dry_fire);
  CHECK(click.shots == 0);
  const FirearmEvents held = RunFor(firearm, Trigger(), kFrame, 0.5f);
  CHECK_FALSE(held.dry_fire);
  (void)firearm.Update(Idle(), kFrame);
  CHECK(firearm.Update(Trigger(), kFrame).dry_fire);
}

TEST_CASE("auto running dry mid-burst does not click") {
  Firearm firearm(AutoSpec());
  const FirearmEvents burst = RunFor(firearm, Trigger(), kFrame, 5.0f);
  CHECK(burst.shots == 31);
  CHECK_FALSE(burst.dry_fire);
  CHECK(firearm.bolt_locked_back());
}

TEST_CASE("auto fires at the cyclic rate independent of the frame time") {
  // 800 rpm: a shot every 75 ms starting immediately, so 14 shots in the
  // first second (t = 0 .. 0.975 s).
  for (const float dt : {1.0f / 30.0f, 1.0f / 60.0f, 1.0f / 144.0f, 0.1f}) {
    CAPTURE(dt);
    Firearm firearm(AutoSpec());
    CHECK(RunFor(firearm, Trigger(), dt, 1.0f).shots == 14);
  }
}

TEST_CASE("the first auto shot of a pull is immediate") {
  Firearm firearm(AutoSpec());
  const FirearmEvents events = firearm.Update(Trigger(), 0.001f);
  REQUIRE(events.shots == 1);
  CHECK(events.shot_age[0] == doctest::Approx(0.001f));
}

TEST_CASE("a long frame fires several shots with their ages") {
  Firearm firearm(AutoSpec());
  const FirearmEvents events = firearm.Update(Trigger(), 0.2f);
  REQUIRE(events.shots == 3);  // t = 0, 0.075, 0.15
  CHECK(events.shot_age[0] == doctest::Approx(0.2f));
  CHECK(events.shot_age[1] == doctest::Approx(0.125f));
  CHECK(events.shot_age[2] == doctest::Approx(0.05f));
  // The next shot is due at t = 0.225, i.e. 25 ms into the next frame.
  const FirearmEvents next = firearm.Update(Trigger(), 0.05f);
  REQUIRE(next.shots == 1);
  CHECK(next.shot_age[0] == doctest::Approx(0.025f));
}

TEST_CASE("an enormous frame reports at most kMaxShotsPerUpdate shots") {
  Firearm firearm(AutoSpec());
  CHECK(firearm.Update(Trigger(), 10.0f).shots == kMaxShotsPerUpdate);
}

TEST_CASE("the fire selector toggles semi and auto") {
  Firearm firearm;
  const FirearmEvents events =
      firearm.Update({.fire_mode_pressed = true}, kFrame);
  CHECK(events.fire_mode_changed);
  CHECK(firearm.fire_mode() == FireMode::kAuto);
  (void)firearm.Update({.fire_mode_pressed = true}, kFrame);
  CHECK(firearm.fire_mode() == FireMode::kSemi);
}

TEST_CASE("reload is refused when nothing would change") {
  Firearm firearm;
  const FirearmEvents events = firearm.Update(Reload(), kFrame);
  CHECK_FALSE(events.reload_started);
  CHECK_FALSE(firearm.reloading());
}

TEST_CASE("reload is refused without a spare magazine") {
  FirearmSpec spec;
  spec.spare_magazines = 0;
  Firearm firearm(spec);
  PullOnce(firearm);
  CHECK_FALSE(firearm.Update(Reload(), kFrame).reload_started);
}

TEST_CASE("a tactical reload ends with capacity + 1") {
  Firearm firearm;
  for (int i = 0; i < 10; ++i) {
    PullOnce(firearm);
  }
  REQUIRE(firearm.rounds_in_magazine() == 20);
  const FirearmEvents start = firearm.Update(Reload(), kFrame);
  CHECK(start.reload_started);
  CHECK(firearm.reloading());
  CHECK_FALSE(firearm.empty_reload());
  const FirearmEvents rest = RunFor(firearm, Idle(), kFrame, 2.3f);
  CHECK(rest.reload_finished);
  CHECK_FALSE(firearm.reloading());
  CHECK(firearm.rounds_in_magazine() == 30);
  CHECK(firearm.round_chambered());
  CHECK(firearm.rounds_loaded() == 31);
  // The partial magazine went back to the pouch; one full one left it.
  const std::span<const int> spares = firearm.spare_magazines();
  CHECK(spares.size() == 4);
  CHECK(std::count(spares.begin(), spares.end(), 20) == 1);
  CHECK(TotalSpareRounds(firearm) == 110);
}

TEST_CASE("an empty reload releases the bolt: capacity rounds in total") {
  Firearm firearm;
  EmptyTheRifle(firearm);
  REQUIRE(firearm.bolt_locked_back());
  CHECK(firearm.Update(Reload(), kFrame).reload_started);
  CHECK(firearm.empty_reload());
  // The tactical time is not enough: the bolt still has to be released.
  CHECK_FALSE(RunFor(firearm, Idle(), kFrame, 2.2f).reload_finished);
  CHECK(RunFor(firearm, Idle(), kFrame, 0.75f).reload_finished);
  CHECK(firearm.rounds_in_magazine() == 29);
  CHECK(firearm.round_chambered());
  CHECK_FALSE(firearm.bolt_locked_back());
  // The empty magazine was discarded.
  CHECK(firearm.spare_magazines().size() == 3);
  CHECK(TotalSpareRounds(firearm) == 90);
}

TEST_CASE("the magazine changes at realistic points during the reload") {
  Firearm firearm;
  EmptyTheRifle(firearm);
  REQUIRE(firearm.Update(Reload(), 0.0f).reload_started);
  const float duration = firearm.spec().empty_reload_seconds;

  (void)firearm.Update(Idle(), 0.2f * duration);
  CHECK(firearm.magazine_inserted());  // still reaching for the release
  CHECK(firearm.reload_fraction() == doctest::Approx(0.2f));

  (void)firearm.Update(Idle(), 0.2f * duration);  // 40 %: magazine out
  CHECK_FALSE(firearm.magazine_inserted());
  CHECK(firearm.rounds_loaded() == 0);

  (void)firearm.Update(Idle(), 0.3f * duration);  // 70 %: new one seated
  CHECK(firearm.magazine_inserted());
  CHECK(firearm.rounds_in_magazine() == 30);
  CHECK_FALSE(firearm.round_chambered());
  CHECK(firearm.bolt_locked_back());

  (void)firearm.Update(Idle(), 0.2f * duration);  // 90 %: bolt released
  CHECK(firearm.round_chambered());
  CHECK(firearm.rounds_in_magazine() == 29);
  CHECK_FALSE(firearm.bolt_locked_back());
  CHECK(firearm.reloading());
}

TEST_CASE("the fullest spare magazine is inserted") {
  FirearmSpec spec;
  spec.spare_magazines = 1;
  Firearm firearm(spec);
  // Reload after 5 shots: the 25-round magazine goes to the pouch.
  for (int i = 0; i < 5; ++i) {
    PullOnce(firearm);
  }
  (void)firearm.Update(Reload(), kFrame);
  (void)RunFor(firearm, Idle(), kFrame, 3.0f);
  REQUIRE(firearm.spare_magazines().size() == 1);
  CHECK(firearm.spare_magazines()[0] == 25);
  // Fire 10 more and reload: the 25 beats the 20 in the rifle.
  for (int i = 0; i < 10; ++i) {
    PullOnce(firearm);
  }
  (void)firearm.Update(Reload(), kFrame);
  (void)RunFor(firearm, Idle(), kFrame, 3.0f);
  CHECK(firearm.rounds_in_magazine() == 25);
  REQUIRE(firearm.spare_magazines().size() == 1);
  CHECK(firearm.spare_magazines()[0] == 20);
}

TEST_CASE("reload is refused while reloading and nothing fires during it") {
  Firearm firearm(AutoSpec());
  PullOnce(firearm);
  REQUIRE(firearm.Update(Reload(), kFrame).reload_started);
  CHECK_FALSE(firearm.Update(Reload(), kFrame).reload_started);
  const FirearmEvents during = RunFor(firearm, Trigger(), kFrame, 2.0f);
  CHECK(during.shots == 0);
  CHECK_FALSE(during.dry_fire);
  CHECK(firearm.reloading());
}

TEST_CASE("can_fire = false blocks shots and dry fire") {
  Firearm firearm;
  const FirearmInput blocked{.trigger_held = true, .can_fire = false};
  const FirearmEvents events = RunFor(firearm, blocked, kFrame, 0.5f);
  CHECK(events.shots == 0);
  CHECK(firearm.rounds_loaded() == 31);
  // The pull was not consumed: still holding when the block ends fires.
  CHECK(firearm.Update(Trigger(), kFrame).shots == 1);

  EmptyTheRifle(firearm);
  CHECK_FALSE(firearm.Update(blocked, kFrame).dry_fire);
}

TEST_CASE("negative dt is treated as zero") {
  Firearm firearm(AutoSpec());
  const FirearmEvents events = firearm.Update(Trigger(), -1.0f);
  REQUIRE(events.shots == 1);
  CHECK(events.shot_age[0] == 0.0f);
}

TEST_CASE("a semi-only weapon starts and stays in semi") {
  FirearmSpec spec;
  spec.fire_modes = FireModeBit(FireMode::kSemi);
  spec.fire_mode = FireMode::kAuto;
  Firearm firearm(spec);
  CHECK(firearm.fire_mode() == FireMode::kSemi);
  const FirearmEvents events =
      firearm.Update({.fire_mode_pressed = true}, kFrame);
  CHECK_FALSE(events.fire_mode_changed);
  CHECK(firearm.fire_mode() == FireMode::kSemi);
  // Holding the trigger fires once only.
  CHECK(RunFor(firearm, Trigger(), kFrame, 1.0f).shots == 1);
}

TEST_CASE("the pistol spec: 17 + 1 rounds, semi only, 3 spares") {
  Firearm pistol(Glock17Spec());
  CHECK(pistol.rounds_loaded() == 18);
  CHECK(pistol.fire_mode() == FireMode::kSemi);
  CHECK(pistol.spare_magazines().size() == 3);
  CHECK(pistol.spec().muzzle_velocity == 375.0f);
  // Fast clicking is bounded by the mechanism's rate, not the frame.
  int shots = 0;
  for (int i = 0; i < 60; ++i) {
    shots += pistol.Update(Trigger(), 1.0f / 240.0f).shots;
    shots += pistol.Update(Idle(), 1.0f / 240.0f).shots;
  }
  // 0.5 s of clicking at 120 pulls per second: 1100 rpm allows 9 + 1.
  CHECK(shots <= 10);
  CHECK(shots >= 8);
}

TEST_CASE("double action fires once per trigger pull") {
  FirearmSpec spec;
  spec.fire_modes = FireModeBit(FireMode::kDoubleAction);
  Firearm firearm(spec);
  CHECK(firearm.fire_mode() == FireMode::kDoubleAction);
  CHECK(RunFor(firearm, Trigger(), kFrame, 1.0f).shots == 1);
  (void)firearm.Update(Idle(), kFrame);
  CHECK(RunFor(firearm, Trigger(), kFrame, 1.0f).shots == 1);
}

TEST_CASE("the selector cycles through every available mode in order") {
  FirearmSpec spec;
  spec.fire_modes = FireModeBit(FireMode::kSemi) |
                    FireModeBit(FireMode::kAuto) |
                    FireModeBit(FireMode::kDoubleAction);
  spec.fire_mode = FireMode::kAuto;
  Firearm firearm(spec);
  CHECK(firearm.fire_mode() == FireMode::kAuto);
  const FirearmInput toggle{.fire_mode_pressed = true};
  CHECK(firearm.Update(toggle, kFrame).fire_mode_changed);
  CHECK(firearm.fire_mode() == FireMode::kDoubleAction);
  (void)firearm.Update(toggle, kFrame);
  CHECK(firearm.fire_mode() == FireMode::kSemi);
  (void)firearm.Update(toggle, kFrame);
  CHECK(firearm.fire_mode() == FireMode::kAuto);
}

TEST_CASE("an empty fire mode set behaves as semi only") {
  FirearmSpec spec;
  spec.fire_modes = 0;
  spec.fire_mode = FireMode::kAuto;
  Firearm firearm(spec);
  CHECK(firearm.fire_mode() == FireMode::kSemi);
  CHECK_FALSE(
      firearm.Update({.fire_mode_pressed = true}, kFrame).fire_mode_changed);
}

TEST_CASE("CancelReload does nothing without a reload") {
  Firearm firearm;
  CHECK_FALSE(firearm.CancelReload());
  CHECK(firearm.rounds_loaded() == 31);
}

TEST_CASE("CancelReload keeps the magazine bookkeeping at every stage") {
  const FirearmSpec spec;
  for (const float fraction : {0.1f, 0.5f, 0.75f, 0.9f}) {
    CAPTURE(fraction);
    for (const bool empty : {false, true}) {
      CAPTURE(empty);
      Firearm firearm(spec);
      const int fired = empty ? spec.magazine_capacity + 1 : 3;
      for (int i = 0; i < fired; ++i) {
        PullOnce(firearm);
      }
      const int loaded_before = firearm.rounds_loaded();
      REQUIRE(firearm.Update(Reload(), 0.0f).reload_started);
      const float seconds =
          empty ? spec.empty_reload_seconds : spec.tactical_reload_seconds;
      (void)firearm.Update(Idle(), fraction * seconds);
      REQUIRE(firearm.reloading());
      CHECK(firearm.CancelReload());
      CHECK_FALSE(firearm.reloading());
      CHECK(firearm.reload_fraction() == 0.0f);
      // No magazine is lost or duplicated: every round is accounted for.
      int carried = firearm.rounds_loaded();
      for (const int spare : firearm.spare_magazines()) {
        carried += spare;
      }
      const int total =
          (spec.spare_magazines + 1) * spec.magazine_capacity + 1 - fired;
      CHECK(carried == total);
      if (fraction < 0.3f) {  // before the pull: unchanged
        CHECK(firearm.magazine_inserted());
        CHECK(firearm.rounds_loaded() == loaded_before);
        CHECK(firearm.spare_magazines().size() == 4);
      } else if (fraction < 0.65f) {  // pulled, not seated: no magazine
        CHECK_FALSE(firearm.magazine_inserted());
        CHECK(firearm.rounds_loaded() == (empty ? 0 : 1));
      } else {  // seated
        CHECK(firearm.magazine_inserted());
        CHECK(firearm.rounds_in_magazine() ==
              spec.magazine_capacity - (empty && fraction >= 0.85f ? 1 : 0));
        CHECK(firearm.bolt_locked_back() == (empty && fraction < 0.85f));
      }
      // A later reload completes normally from that state.
      if (firearm.Update(Reload(), 0.0f).reload_started) {
        (void)firearm.Update(Idle(), 5.0f);
      }
      CHECK_FALSE(firearm.reloading());
      CHECK(firearm.magazine_inserted());
      CHECK(firearm.round_chambered());
      CHECK_FALSE(firearm.bolt_locked_back());
    }
  }
}

TEST_CASE("a weapon that is not functional neither fires nor reloads") {
  FirearmSpec spec = Glock17Spec();
  spec.functional = false;
  Firearm pistol(spec);
  const FirearmEvents pulled = pistol.Update(Trigger(), kFrame);
  CHECK(pulled.shots == 0);
  CHECK_FALSE(pulled.dry_fire);
  (void)pistol.Update(Idle(), kFrame);
  CHECK_FALSE(pistol.Update(Reload(), kFrame).reload_started);
  CHECK(pistol.round_chambered());
  CHECK(pistol.rounds_in_magazine() == 17);
}

TEST_CASE("a weapon without a magazine fires its chambered round only") {
  FirearmSpec spec = Glock17Spec();
  spec.magazine_capacity = 0;
  Firearm pistol(spec);
  CHECK_FALSE(pistol.magazine_inserted());
  CHECK(pistol.rounds_loaded() == 1);
  CHECK(pistol.spare_magazines().empty());
  CHECK(PullOnce(pistol) == 1);
  CHECK(pistol.bolt_locked_back());
  CHECK(PullOnce(pistol) == 0);
  CHECK_FALSE(pistol.Update(Reload(), kFrame).reload_started);
}

TEST_CASE("Refit keeps the state the new parts allow") {
  const FirearmSpec stock = Glock17Spec();
  FirearmSpec extended = stock;
  extended.magazine_capacity = 33;
  FirearmSpec bare = stock;
  bare.magazine_capacity = 0;

  SUBCASE("a bigger magazine keeps the rounds") {
    Firearm pistol(stock);
    (void)PullOnce(pistol);
    pistol.Refit(extended);
    CHECK(pistol.spec().magazine_capacity == 33);
    CHECK(pistol.rounds_in_magazine() == 16);
    CHECK(pistol.round_chambered());
    CHECK(pistol.spare_magazines().size() == 3);
  }
  SUBCASE("a smaller magazine drops the rounds above its capacity") {
    Firearm pistol(extended);
    FirearmSpec small = stock;
    small.magazine_capacity = 10;
    pistol.Refit(small);
    CHECK(pistol.rounds_in_magazine() == 10);
    CHECK(pistol.round_chambered());
    for (const int rounds : pistol.spare_magazines()) {
      CHECK(rounds == 10);
    }
  }
  SUBCASE("taking the magazine off leaves the chambered round, no reload") {
    Firearm pistol(stock);
    pistol.Refit(bare);
    CHECK_FALSE(pistol.magazine_inserted());
    CHECK(pistol.rounds_in_magazine() == 0);
    CHECK(pistol.rounds_loaded() == 1);
    CHECK_FALSE(pistol.Update(Reload(), kFrame).reload_started);
    CHECK(PullOnce(pistol) == 1);
    CHECK(PullOnce(pistol) == 0);
    // Spares stay with the weapon for when a magazine is fitted again.
    CHECK(pistol.spare_magazines().size() == 3);
  }
  SUBCASE("a magazine fitted again goes in full") {
    Firearm pistol(stock);
    pistol.Refit(bare);
    pistol.Refit(stock);
    CHECK(pistol.magazine_inserted());
    CHECK(pistol.rounds_in_magazine() == 17);
    CHECK(pistol.round_chambered());
  }
  SUBCASE("a running reload is cancelled") {
    Firearm pistol(stock);
    (void)PullOnce(pistol);
    REQUIRE(pistol.Update(Reload(), kFrame).reload_started);
    pistol.Refit(extended);
    CHECK_FALSE(pistol.reloading());
    CHECK(TotalSpareRounds(pistol) == 3 * 17);
  }
  SUBCASE("the fire mode stays if offered, else the start mode") {
    FirearmSpec select = stock;
    select.fire_modes =
        FireModeBit(FireMode::kSemi) | FireModeBit(FireMode::kAuto);
    Firearm pistol(select);
    (void)pistol.Update({.fire_mode_pressed = true}, kFrame);
    REQUIRE(pistol.fire_mode() == FireMode::kAuto);
    pistol.Refit(select);
    CHECK(pistol.fire_mode() == FireMode::kAuto);
    pistol.Refit(stock);
    CHECK(pistol.fire_mode() == FireMode::kSemi);
  }
  SUBCASE("a locked bolt stays locked") {
    Firearm pistol(stock);
    while (pistol.round_chambered()) {
      (void)PullOnce(pistol);
    }
    REQUIRE(pistol.bolt_locked_back());
    pistol.Refit(extended);
    CHECK(pistol.bolt_locked_back());
    CHECK_FALSE(pistol.round_chambered());
  }
  SUBCASE("losing a vital part stops the trigger") {
    Firearm pistol(stock);
    FirearmSpec broken = stock;
    broken.functional = false;
    pistol.Refit(broken);
    CHECK(pistol.Update(Trigger(), kFrame).shots == 0);
    (void)pistol.Update(Idle(), kFrame);
    pistol.Refit(stock);
    CHECK(pistol.Update(Trigger(), kFrame).shots == 1);
  }
}

}  // namespace
}  // namespace blue::weapon
