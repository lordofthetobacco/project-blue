#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/interpolation.h"

namespace blue::net {
namespace {

constexpr double kTick = 1.0 / 60.0;

InterpSample At(uint32_t tick, float x) {
  return InterpSample{.tick = tick,
                      .position = glm::vec3(x, 0.0f, 0.0f),
                      .velocity = glm::vec3(60.0f, 0.0f, 0.0f)};
}

TEST_CASE("view time arithmetic wraps and splits fractions") {
  const ViewTime t = AddTicks(ViewTime{.tick = 10}, 2.25);
  CHECK(t.tick == 12);
  CHECK(t.fraction == doctest::Approx(0.25));
  const ViewTime back = AddTicks(t, -3.5);
  CHECK(back.tick == 8);
  CHECK(back.fraction == doctest::Approx(0.75));
  CHECK(TicksBetween(t, back) == doctest::Approx(3.5));
  CHECK(AddTicks(ViewTime{.tick = 0}, -0.5).tick == 0xFFFFFFFFu);
  CHECK(ViewTime{.tick = 1, .fraction = 0.5}.fraction_256() == 128);
  CHECK(ViewTime{.tick = 1, .fraction = 0.9999}.fraction_256() == 255);
}

TEST_CASE("interpolation buffer orders, replaces and evicts samples") {
  InterpolationBuffer buffer;
  CHECK_FALSE(buffer.Sample(ViewTime{.tick = 5}, 6.0).has_value());
  buffer.Push(At(10, 1.0f));
  buffer.Push(At(12, 3.0f));
  buffer.Push(At(11, 2.0f));  // Late: inserted in place.
  buffer.Push(At(12, 4.0f));  // Duplicate tick: replaces.
  CHECK(buffer.size() == 3);
  CHECK(buffer.newest().position.x == 4.0f);
  for (uint32_t t = 13; t < 13 + InterpolationBuffer::kCapacity; ++t) {
    buffer.Push(At(t, static_cast<float>(t)));
  }
  CHECK(buffer.size() == InterpolationBuffer::kCapacity);
  buffer.Push(At(1, 0.0f));  // Older than everything kept: dropped.
  CHECK(buffer.size() == InterpolationBuffer::kCapacity);
}

TEST_CASE("interpolation blends, extrapolates briefly, then holds") {
  InterpolationBuffer buffer;
  buffer.Push(At(100, 0.0f));
  buffer.Push(At(102, 2.0f));
  // Before the oldest: the oldest.
  CHECK(buffer.Sample(ViewTime{.tick = 90}, 6.0)->position.x == 0.0f);
  const std::optional<InterpSample> mid =
      buffer.Sample(ViewTime{.tick = 101, .fraction = 0.5}, 6.0);
  REQUIRE(mid.has_value());
  CHECK(mid->position.x == doctest::Approx(1.5f));
  CHECK(mid->tick == 100);
  // Past the newest: velocity 60 m/s = 1 m per tick, for at most 6 ticks.
  CHECK(buffer.Sample(ViewTime{.tick = 104}, 6.0)->position.x ==
        doctest::Approx(4.0f));
  CHECK(buffer.Sample(ViewTime{.tick = 200}, 6.0)->position.x ==
        doctest::Approx(8.0f));
  // At-rest samples do not extrapolate.
  InterpSample rest = At(103, 5.0f);
  rest.at_rest = true;
  buffer.Push(rest);
  CHECK(buffer.Sample(ViewTime{.tick = 110}, 6.0)->position.x == 5.0f);
}

TEST_CASE("interpolation takes the short way round for yaw and slerps") {
  InterpolationBuffer buffer;
  constexpr float kPi = std::numbers::pi_v<float>;
  InterpSample a = At(0, 0.0f);
  a.yaw = 2.0f * kPi - 0.1f;
  a.rotation = glm::angleAxis(0.0f, glm::vec3(0.0f, 1.0f, 0.0f));
  InterpSample b = At(2, 0.0f);
  b.yaw = 0.1f;
  b.rotation = glm::angleAxis(kPi / 2.0f, glm::vec3(0.0f, 1.0f, 0.0f));
  buffer.Push(a);
  buffer.Push(b);
  const InterpSample mid = *buffer.Sample(ViewTime{.tick = 1}, 6.0);
  CHECK(std::remainder(mid.yaw, 2.0f * kPi) == doctest::Approx(0.0f));
  const float angle = glm::angle(mid.rotation);
  CHECK(angle == doctest::Approx(kPi / 4.0f).epsilon(0.001));
}

TEST_CASE("interpolation clock lags the newest snapshot by its delay") {
  InterpolationClock clock;
  CHECK(clock.Advance(0.0).tick == 0);
  double now = 0.0;
  for (uint32_t tick = 500; tick < 800; ++tick) {
    clock.OnSnapshot(tick, now);
    const ViewTime before = clock.render_time();
    const ViewTime render = clock.Advance(now);
    CHECK(TicksBetween(render, before) >= 0.0);  // Never backwards.
    now += kTick;
  }
  CHECK(clock.jitter_ticks() < 0.1);
  CHECK(clock.delay_ticks() == doctest::Approx(2.0).epsilon(0.1));
  const double lag = TicksBetween(ViewTime{.tick = 799}, clock.render_time());
  CHECK(lag == doctest::Approx(2.0).epsilon(0.25));
}

TEST_CASE("interpolation clock widens its delay with jitter") {
  InterpolationClock clock;
  double now = 0.0;
  for (uint32_t tick = 0; tick < 600; ++tick) {
    // Every other snapshot arrives 2 ticks late.
    const double arrival = now + (tick % 2 == 0 ? 0.0 : 2 * kTick);
    clock.OnSnapshot(tick, arrival);
    (void)clock.Advance(now);
    now += kTick;
  }
  CHECK(clock.jitter_ticks() > 0.5);
  CHECK(clock.delay_ticks() > 3.0);
  CHECK(clock.delay_ticks() <= clock.config().max_delay_ticks);
}

}  // namespace
}  // namespace blue::net
