#include "net/interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/sequence.h"
#include "sim/user_command.h"

namespace blue::net {
namespace {

// Steering of the render time: rate change per tick of error.
constexpr double kSteerGain = 0.05;
// The newest server tick is not estimated further than this past its
// arrival (snapshots stopped).
constexpr double kMaxServerLeadTicks = 30.0;

// Returns the yaw from `a` towards `b` by `t` along the shorter arc.
float LerpYaw(float a, float b, float t) {
  constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;
  float delta = std::remainder(b - a, kTwoPi);
  return a + delta * t;
}

}  // namespace

uint8_t ViewTime::fraction_256() const {
  return static_cast<uint8_t>(
      std::clamp(std::floor(fraction * 256.0), 0.0, 255.0));
}

ViewTime AddTicks(ViewTime a, double b) {
  const double total = a.fraction + b;
  const double whole = std::floor(total);
  ViewTime result;
  result.tick = a.tick + static_cast<uint32_t>(static_cast<int64_t>(whole));
  result.fraction = total - whole;
  return result;
}

double TicksBetween(ViewTime a, ViewTime b) {
  const auto ticks = static_cast<int32_t>(a.tick - b.tick);
  return static_cast<double>(ticks) + (a.fraction - b.fraction);
}

void InterpolationBuffer::Push(const InterpSample& sample) {
  // Find the insertion point from the newest end (samples mostly arrive in
  // order).
  size_t at = size_;
  while (at > 0 && TickGreater(samples_[at - 1].tick, sample.tick)) {
    --at;
  }
  if (at > 0 && samples_[at - 1].tick == sample.tick) {
    samples_[at - 1] = sample;
    return;
  }
  if (size_ == samples_.size()) {
    if (at == 0) {
      return;  // Older than everything kept.
    }
    // Drop the oldest.
    std::move(samples_.begin() + 1,
              samples_.begin() + static_cast<ptrdiff_t>(at), samples_.begin());
    samples_[at - 1] = sample;
    return;
  }
  std::move_backward(samples_.begin() + static_cast<ptrdiff_t>(at),
                     samples_.begin() + static_cast<ptrdiff_t>(size_),
                     samples_.begin() + static_cast<ptrdiff_t>(size_ + 1));
  samples_[at] = sample;
  ++size_;
}

std::optional<InterpSample> InterpolationBuffer::Sample(
    ViewTime time, double max_extrapolation_ticks) const {
  if (size_ == 0) {
    return std::nullopt;
  }
  const InterpSample& oldest = samples_[0];
  if (TicksBetween(time, ViewTime{.tick = oldest.tick}) <= 0.0) {
    return oldest;
  }
  const InterpSample& last = samples_[size_ - 1];
  const double past_newest = TicksBetween(time, ViewTime{.tick = last.tick});
  if (past_newest >= 0.0) {
    InterpSample result = last;
    const double ticks = std::min(past_newest, max_extrapolation_ticks);
    if (!last.at_rest && ticks > 0.0) {
      result.position +=
          last.velocity * static_cast<float>(ticks * sim::kTickSeconds);
    }
    return result;
  }
  // samples_[i] is the first sample newer than `time`.
  size_t i = 1;
  while (TicksBetween(time, ViewTime{.tick = samples_[i].tick}) >= 0.0) {
    ++i;
  }
  const InterpSample& a = samples_[i - 1];
  const InterpSample& b = samples_[i];
  const double span = static_cast<double>(b.tick - a.tick);
  const auto t =
      static_cast<float>(TicksBetween(time, ViewTime{.tick = a.tick}) / span);
  InterpSample result = a;
  result.position = glm::mix(a.position, b.position, t);
  result.velocity = glm::mix(a.velocity, b.velocity, t);
  result.yaw = LerpYaw(a.yaw, b.yaw, t);
  result.pitch = a.pitch + (b.pitch - a.pitch) * t;
  result.rotation = glm::slerp(a.rotation, b.rotation, t);
  return result;
}

void InterpolationClock::Reset() { *this = InterpolationClock(config_); }

double InterpolationClock::delay_ticks() const {
  return std::clamp(
      config_.base_delay_ticks + config_.jitter_factor * jitter_ticks_,
      config_.min_delay_ticks, config_.max_delay_ticks);
}

ViewTime InterpolationClock::ServerNow(double now_seconds) const {
  const double elapsed =
      std::clamp((now_seconds - newest_arrival_) / config_.tick_seconds, 0.0,
                 kMaxServerLeadTicks);
  return AddTicks(ViewTime{.tick = newest_tick_}, elapsed);
}

void InterpolationClock::OnSnapshot(uint32_t server_tick, double now_seconds) {
  if (!started_) {
    started_ = true;
    newest_tick_ = server_tick;
    newest_arrival_ = now_seconds;
    last_advance_ = now_seconds;
    render_ = AddTicks(ViewTime{.tick = server_tick}, -delay_ticks());
    return;
  }
  if (!TickGreater(server_tick, newest_tick_)) {
    return;  // Late or duplicate: no timing information about the newest.
  }
  const double expected =
      newest_arrival_ +
      static_cast<double>(server_tick - newest_tick_) * config_.tick_seconds;
  const double deviation =
      std::abs(now_seconds - expected) / config_.tick_seconds;
  jitter_ticks_ += config_.jitter_smoothing * (deviation - jitter_ticks_);
  newest_tick_ = server_tick;
  newest_arrival_ = now_seconds;
}

ViewTime InterpolationClock::Advance(double now_seconds) {
  if (!started_) {
    return render_;
  }
  const double elapsed_ticks =
      std::max(now_seconds - last_advance_, 0.0) / config_.tick_seconds;
  last_advance_ = now_seconds;
  const ViewTime target = AddTicks(ServerNow(now_seconds), -delay_ticks());
  // The error against where real time alone would put the render time.
  const double error = TicksBetween(target, AddTicks(render_, elapsed_ticks));
  if (error > config_.snap_ticks) {
    render_ = target;
    return render_;
  }
  double rate = 1.0 + std::clamp(error * kSteerGain, -config_.max_catch_up,
                                 config_.max_catch_up);
  if (error < -config_.snap_ticks) {
    rate = 0.0;  // Far ahead: wait for the server.
  }
  render_ = AddTicks(render_, elapsed_ticks * rate);
  return render_;
}

}  // namespace blue::net
