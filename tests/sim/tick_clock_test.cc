#include "sim/tick_clock.h"

#include <doctest/doctest.h>

#include <limits>

#include "sim/user_command.h"

namespace blue::sim {
namespace {

TEST_CASE("one tick per tick-long frame") {
  TickClock clock;
  int ticks = 0;
  for (int i = 0; i < 60; ++i) {
    const TickAdvance advance = clock.Advance(kTickSeconds);
    CHECK(advance.ticks == 1);
    ticks += advance.ticks;
  }
  CHECK(ticks == 60);
}

TEST_CASE("short frames accumulate into ticks") {
  TickClock clock;
  const float quarter = 0.25f * kTickSeconds;
  CHECK(clock.Advance(quarter).ticks == 0);
  CHECK(clock.Advance(quarter).ticks == 0);
  const TickAdvance half = clock.Advance(0.0f);
  CHECK(half.ticks == 0);
  CHECK(half.alpha == doctest::Approx(0.5f));
  CHECK(clock.Advance(quarter).ticks == 0);
  const TickAdvance full = clock.Advance(quarter);
  CHECK(full.ticks == 1);
  CHECK(full.alpha == doctest::Approx(0.0f).epsilon(1e-4));
}

TEST_CASE("the alpha is the remainder of the next tick") {
  TickClock clock;
  const TickAdvance advance = clock.Advance(2.5f * kTickSeconds);
  CHECK(advance.ticks == 2);
  CHECK(advance.alpha == doctest::Approx(0.5f).epsilon(1e-4));
}

TEST_CASE("a long frame runs at most four ticks and drops the backlog") {
  TickClock clock;
  const TickAdvance first = clock.Advance(10.5f * kTickSeconds);
  CHECK(first.ticks == TickClock::kMaxTicksPerAdvance);
  CHECK(first.alpha == doctest::Approx(0.5f).epsilon(1e-3));
  // The dropped ticks are not caught up later.
  CHECK(clock.Advance(0.5f * kTickSeconds).ticks == 1);
  CHECK(clock.Advance(0.5f * kTickSeconds).ticks == 0);
}

TEST_CASE("a frame is clamped to a quarter second") {
  TickClock clock;
  // 10 s would be 600 ticks; only 0.25 s = 15 ticks count, of which 4 run
  // and the rest is dropped.
  CHECK(clock.Advance(10.0f).ticks == 4);
  CHECK(clock.Advance(kTickSeconds).ticks <= 2);
}

TEST_CASE("bad frame times add nothing") {
  TickClock clock;
  CHECK(clock.Advance(0.5f * kTickSeconds).ticks == 0);
  CHECK(clock.Advance(-1.0f).ticks == 0);
  CHECK(clock.Advance(std::numeric_limits<float>::quiet_NaN()).ticks == 0);
  const TickAdvance advance = clock.Advance(0.0f);
  CHECK(advance.alpha == doctest::Approx(0.5f));
}

TEST_CASE("the time scale speeds up or slows down the ticks") {
  CHECK(TickClock().time_scale() == 1.0f);

  SUBCASE("faster") {
    TickClock clock;
    clock.SetTimeScale(1.1f);
    CHECK(clock.time_scale() == doctest::Approx(1.1f));
    int ticks = 0;
    for (int i = 0; i < 600; ++i) {
      ticks += clock.Advance(kTickSeconds).ticks;
    }
    CHECK(ticks == 660);
  }
  SUBCASE("slower") {
    TickClock clock;
    clock.SetTimeScale(0.9f);
    int ticks = 0;
    for (int i = 0; i < 600; ++i) {
      ticks += clock.Advance(kTickSeconds).ticks;
    }
    CHECK(ticks == 540);
  }
  SUBCASE("clamped") {
    TickClock clock;
    clock.SetTimeScale(5.0f);
    CHECK(clock.time_scale() == doctest::Approx(TickClock::kMaxTimeScale));
    clock.SetTimeScale(0.0f);
    CHECK(clock.time_scale() == doctest::Approx(TickClock::kMinTimeScale));
    clock.SetTimeScale(std::numeric_limits<float>::infinity());
    CHECK(clock.time_scale() == 1.0f);
  }
}

TEST_CASE("Reset forgets the accumulated time") {
  TickClock clock;
  clock.SetTimeScale(1.05f);
  CHECK(clock.Advance(0.75f * kTickSeconds).ticks == 0);
  clock.Reset();
  CHECK(clock.Advance(0.0f).alpha == 0.0f);
  CHECK(clock.time_scale() == doctest::Approx(1.05f));
}

}  // namespace
}  // namespace blue::sim
