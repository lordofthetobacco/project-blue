#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

#include "net/clock_sync.h"

namespace blue::net {
namespace {

constexpr double kTick = 1.0 / 60.0;

TEST_CASE("clock sync starts a round trip plus the target ahead") {
  ClockSync clock;
  for (int i = 0; i < 10; ++i) {
    clock.AddRoundTrip(0.1);  // 6 ticks
  }
  CHECK(clock.round_trip() == doctest::Approx(0.1));
  CHECK(clock.jitter() == doctest::Approx(0.0));
  CHECK(clock.target_depth() == doctest::Approx(2.0f));
  CHECK(clock.StartTick(1000) == 1000 + 6 + 2);
  CHECK(clock.StartTick(0xFFFFFFFEu) == 6u);  // Wraps.
}

TEST_CASE("clock sync ignores bad round trips and adds jitter to target") {
  ClockSync clock;
  clock.AddRoundTrip(-1.0);
  clock.AddRoundTrip(std::nan(""));
  CHECK(clock.round_trip() == 0.0);
  for (int i = 0; i < 200; ++i) {
    clock.AddRoundTrip(i % 2 == 0 ? 0.1 : 0.1 + 4 * kTick);
  }
  CHECK(clock.jitter() > kTick);
  CHECK(clock.target_depth() > 3.0f);
  CHECK(clock.target_depth() <= 2.0f + 4.0f);
}

TEST_CASE("clock sync steers the depth with a dead band") {
  ClockSync clock;
  clock.AddRoundTrip(0.1);
  const uint32_t server = 5000;
  const uint32_t client = clock.StartTick(server);
  // On target: no dilation.
  ClockAdjust adjust = clock.OnSnapshot(server, 2, client);
  CHECK(adjust.time_scale == 1.0f);
  CHECK_FALSE(adjust.resync_tick.has_value());
  // Slightly off stays inside the dead band.
  adjust = clock.OnSnapshot(server + 1, 3, client + 1);
  CHECK(adjust.time_scale == 1.0f);
  // Persistently too deep: slow down; too shallow: speed up.
  for (int i = 0; i < 60; ++i) {
    adjust = clock.OnSnapshot(server + 2 + i, 8, client + 2 + i);
  }
  CHECK(adjust.time_scale < 1.0f);
  CHECK(adjust.time_scale >= 0.95f);
  for (int i = 0; i < 120; ++i) {
    adjust = clock.OnSnapshot(server + 100 + i, -3, client + 100 + i);
  }
  CHECK(adjust.time_scale > 1.0f);
  CHECK(adjust.time_scale <= 1.05f);
  CHECK(clock.resyncs() == 0);
}

TEST_CASE("clock sync resyncs on a large error and settles afterwards") {
  ClockSync clock;
  clock.AddRoundTrip(0.1);
  const uint32_t server = 7000;
  // The client runs behind the server: resync forward.
  ClockAdjust adjust = clock.OnSnapshot(server, -20, server - 3);
  REQUIRE(adjust.resync_tick.has_value());
  CHECK(*adjust.resync_tick == clock.StartTick(server));
  CHECK(clock.resyncs() == 1);
  // 30 ticks too far ahead (500 ms > 250 ms): resync.
  const uint32_t start = clock.StartTick(server);
  adjust = clock.OnSnapshot(server + 1, 31, start + 31);
  CHECK(adjust.resync_tick.has_value());
  CHECK(clock.resyncs() == 2);
  // Depth reports for ticks before the resync tick are ignored.
  const uint32_t resync = *adjust.resync_tick;
  adjust = clock.OnSnapshot(server + 2, 20, resync + 1);
  CHECK_FALSE(adjust.resync_tick.has_value());
  CHECK(adjust.time_scale == 1.0f);
}

TEST_CASE("clock sync ignores depth reports from before the start") {
  ClockSync clock;
  clock.AddRoundTrip(0.1);
  const uint32_t server = 9000;
  const uint32_t start = clock.StartTick(server);
  clock.Start(start);
  // The server ran ticks before `start` without commands: very negative
  // depth, but no resync and no dilation.
  ClockAdjust adjust = clock.OnSnapshot(server, -20, start);
  CHECK_FALSE(adjust.resync_tick.has_value());
  CHECK(adjust.time_scale == 1.0f);
  // From the start tick on the reports count.
  for (uint32_t i = 0; i < 60; ++i) {
    adjust = clock.OnSnapshot(start + i, 8, start + 10 + i);
  }
  CHECK(adjust.time_scale < 1.0f);
  CHECK(clock.resyncs() == 0);
}

TEST_CASE("clock sync reset forgets everything") {
  ClockSync clock;
  clock.AddRoundTrip(0.2);
  (void)clock.OnSnapshot(10, -30, 5);
  clock.Reset();
  CHECK(clock.round_trip() == 0.0);
  CHECK(clock.resyncs() == 0);
  CHECK(clock.time_scale() == 1.0f);
}

}  // namespace
}  // namespace blue::net
