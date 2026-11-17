#include "weapon/firearm.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <numeric>
#include <span>

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

}  // namespace
}  // namespace blue::weapon
