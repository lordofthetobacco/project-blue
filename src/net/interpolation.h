#ifndef BLUE_NET_INTERPOLATION_H_
#define BLUE_NET_INTERPOLATION_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

namespace blue::net {

// Snapshot interpolation of remote players and world objects: each keeps a
// short buffer of states stamped with their server tick, and everything is
// shown at one common render time a little behind the newest snapshot.

// One remote state at the end of a server tick. Players use `yaw` and
// `pitch`, objects `rotation`; the discrete fields (`flags`, `health`,
// `weapon_key`, `at_rest`) are not blended.
struct InterpSample {
  uint32_t tick = 0;
  glm::vec3 position{0.0f};
  glm::vec3 velocity{0.0f};
  float yaw = 0.0f;
  float pitch = 0.0f;
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  uint8_t flags = 0;
  int16_t health = 0;
  uint32_t weapon_key = 0;
  bool at_rest = false;

  friend bool operator==(const InterpSample&, const InterpSample&) = default;
};

// A server time in ticks: a whole tick plus a fraction in [0, 1).
struct ViewTime {
  uint32_t tick = 0;
  double fraction = 0.0;

  // The fraction in 1/256 of a tick, as ClientInput::view_fraction.
  [[nodiscard]] uint8_t fraction_256() const;
};

// Returns `a` + `b` ticks (b may be negative).
[[nodiscard]] ViewTime AddTicks(ViewTime a, double b);
// Returns `a` - `b` in ticks.
[[nodiscard]] double TicksBetween(ViewTime a, ViewTime b);

// The states of one remote entity, newest kept (kCapacity). Samples arrive
// in any order; a sample older than the newest stored one is inserted in
// place, a duplicate tick replaces the stored one. Fixed size. Copyable.
class InterpolationBuffer {
 public:
  static constexpr int kCapacity = 32;

  InterpolationBuffer() = default;
  InterpolationBuffer(const InterpolationBuffer&) = default;
  InterpolationBuffer& operator=(const InterpolationBuffer&) = default;

  void Push(const InterpSample& sample);
  void Clear() { size_ = 0; }
  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] size_t size() const { return size_; }
  // The newest sample; the buffer must not be empty.
  [[nodiscard]] const InterpSample& newest() const {
    return samples_[size_ - 1];
  }

  // Returns the state at `time`: blended between the samples around it
  // (linear position and velocity, shortest-arc yaw, linear pitch, slerped
  // rotation, discrete fields of the older sample); before the oldest
  // sample, the oldest; after the newest, extrapolated along its velocity
  // for at most `max_extrapolation_ticks`, then held. Its `tick` is the
  // tick of the older sample. nullopt when empty.
  [[nodiscard]] std::optional<InterpSample> Sample(
      ViewTime time, double max_extrapolation_ticks) const;

 private:
  std::array<InterpSample, kCapacity> samples_{};  // oldest first
  size_t size_ = 0;
};

// Tunables of InterpolationClock.
struct InterpolationConfig {
  double tick_seconds = 1.0 / 60.0;
  // Delay behind the newest snapshot without jitter, and its range.
  double base_delay_ticks = 2.0;
  double min_delay_ticks = 2.0;
  double max_delay_ticks = 12.0;
  // Delay added per tick of measured arrival jitter.
  double jitter_factor = 2.0;
  // Weight of one arrival in the jitter average.
  double jitter_smoothing = 0.05;
  // How fast the render time may run off real time to catch up (fraction).
  double max_catch_up = 0.1;
  // Errors beyond this many ticks jump (forward only; backwards it waits).
  double snap_ticks = 8.0;
  // Most extrapolation past the newest sample: 100 ms.
  double max_extrapolation_ticks = 6.0;
};

// The render time of the remote world: the newest server tick (as of now,
// estimated from its arrival) minus an adaptive delay of base + measured
// snapshot arrival jitter, clamped. It never runs backwards; it advances
// with real time and is steered towards that target by at most
// `max_catch_up`. Copyable.
class InterpolationClock {
 public:
  InterpolationClock() = default;
  explicit InterpolationClock(const InterpolationConfig& config)
      : config_(config) {}
  InterpolationClock(const InterpolationClock&) = default;
  InterpolationClock& operator=(const InterpolationClock&) = default;

  // Forgets everything (a new connection).
  void Reset();

  // Takes in the arrival of a snapshot of `server_tick` at `now_seconds`.
  void OnSnapshot(uint32_t server_tick, double now_seconds);

  // Advances the render time to `now_seconds` and returns it. Before the
  // first snapshot it stays at tick 0.
  ViewTime Advance(double now_seconds);

  [[nodiscard]] ViewTime render_time() const { return render_; }
  [[nodiscard]] bool started() const { return started_; }
  // Current delay target, in ticks.
  [[nodiscard]] double delay_ticks() const;
  // Measured arrival jitter, in ticks.
  [[nodiscard]] double jitter_ticks() const { return jitter_ticks_; }
  [[nodiscard]] const InterpolationConfig& config() const { return config_; }

 private:
  // The newest server tick estimated at `now_seconds`.
  [[nodiscard]] ViewTime ServerNow(double now_seconds) const;

  InterpolationConfig config_;
  bool started_ = false;
  uint32_t newest_tick_ = 0;
  double newest_arrival_ = 0.0;
  double jitter_ticks_ = 0.0;
  double last_advance_ = 0.0;
  ViewTime render_;
};

}  // namespace blue::net

#endif  // BLUE_NET_INTERPOLATION_H_
