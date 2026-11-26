#include "net/clock_sync.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "net/sequence.h"

namespace blue::net {

void ClockSync::Reset() { *this = ClockSync(config_); }

void ClockSync::AddRoundTrip(double seconds) {
  if (!std::isfinite(seconds) || seconds <= 0.0) {
    return;
  }
  if (!has_round_trip_) {
    round_trip_ = seconds;
    jitter_ = 0.0;
    has_round_trip_ = true;
    return;
  }
  const double weight = config_.smoothing;
  jitter_ += weight * (std::abs(seconds - round_trip_) - jitter_);
  round_trip_ += weight * (seconds - round_trip_);
}

float ClockSync::target_depth() const {
  const float jitter_ticks = static_cast<float>(jitter_ / config_.tick_seconds);
  return config_.target_depth +
         std::min(jitter_ticks, config_.max_jitter_ticks);
}

uint32_t ClockSync::RoundTripTicks() const {
  return static_cast<uint32_t>(std::ceil(round_trip_ / config_.tick_seconds));
}

uint32_t ClockSync::StartTick(uint32_t server_tick) const {
  return server_tick + RoundTripTicks() +
         static_cast<uint32_t>(std::ceil(target_depth()));
}

void ClockSync::Start(uint32_t start_tick) {
  has_depth_ = false;
  time_scale_ = 1.0f;
  settle_tick_ = start_tick;
}

ClockAdjust ClockSync::Resync(uint32_t tick) {
  ++resyncs_;
  has_depth_ = false;
  time_scale_ = 1.0f;
  settle_tick_ = tick;
  return ClockAdjust{.time_scale = time_scale_, .resync_tick = tick};
}

ClockAdjust ClockSync::OnSnapshot(uint32_t server_tick,
                                  int8_t input_buffer_depth,
                                  uint32_t next_client_tick) {
  const double resync_ticks = config_.resync_seconds / config_.tick_seconds;
  // A client tick the server has already simulated can never be on time.
  if (!TickGreater(next_client_tick, server_tick)) {
    return Resync(StartTick(server_tick));
  }
  // Where the client runs against where it should: the lead over the
  // snapshot's tick versus the round trip plus the target.
  const double lead = static_cast<double>(next_client_tick - server_tick);
  const double wanted = static_cast<double>(RoundTripTicks()) + target_depth();
  if (std::abs(lead - wanted) > resync_ticks) {
    return Resync(StartTick(server_tick));
  }
  if (settle_tick_.has_value()) {
    if (TickGreater(*settle_tick_, server_tick)) {
      return ClockAdjust{.time_scale = time_scale_};
    }
    settle_tick_.reset();
  }

  const float depth = static_cast<float>(input_buffer_depth);
  if (!has_depth_) {
    depth_ = depth;
    has_depth_ = true;
  } else {
    depth_ += config_.smoothing * (depth - depth_);
  }
  const float error = depth_ - target_depth();
  if (std::abs(static_cast<double>(error)) > resync_ticks) {
    return Resync(StartTick(server_tick));
  }
  if (std::abs(error) <= config_.dead_band) {
    time_scale_ = 1.0f;
  } else {
    // Too deep: the client runs too far ahead, so it slows down.
    const float beyond =
        error > 0.0f ? error - config_.dead_band : error + config_.dead_band;
    time_scale_ = 1.0f - std::clamp(beyond * config_.gain, -config_.max_adjust,
                                    config_.max_adjust);
  }
  return ClockAdjust{.time_scale = time_scale_};
}

}  // namespace blue::net
