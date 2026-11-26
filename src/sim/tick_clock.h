#ifndef BLUE_SIM_TICK_CLOCK_H_
#define BLUE_SIM_TICK_CLOCK_H_

namespace blue::sim {

// Result of one TickClock::Advance.
struct TickAdvance {
  // Whole ticks to run now, [0, TickClock::kMaxTicksPerAdvance].
  int ticks = 0;
  // Fraction of the next tick already elapsed, [0, 1], for interpolating
  // between the last two ticks.
  float alpha = 0.0f;
};

// Turns variable frame times into whole simulation ticks of kTickSeconds,
// like PhysicsWorld::Update does for its own steps: time accumulates (at most
// 0.25 s per call), whole ticks are due, at most 4 run per call and a larger
// backlog is dropped rather than carried. A time scale slightly above or below
// 1 lets a networked client run a little ahead of or behind real time.
// Copyable. Not thread-safe.
class TickClock {
 public:
  static constexpr int kMaxTicksPerAdvance = 4;
  static constexpr float kMinTimeScale = 0.9f;
  static constexpr float kMaxTimeScale = 1.1f;

  TickClock() = default;
  TickClock(const TickClock&) = default;
  TickClock& operator=(const TickClock&) = default;

  // Adds `frame_seconds` of real time (times the time scale) and returns the
  // ticks that became due. Non-positive and NaN times add nothing.
  TickAdvance Advance(float frame_seconds);

  // Sets how fast simulated time runs relative to real time, clamped to
  // [kMinTimeScale, kMaxTimeScale]. Non-finite values reset it to 1.
  void SetTimeScale(float scale);
  [[nodiscard]] float time_scale() const;

  // Forgets the accumulated time; the time scale is kept.
  void Reset();

 private:
  double accumulator_ = 0.0;  // Seconds not yet turned into ticks.
  double time_scale_ = 1.0;
};

}  // namespace blue::sim

#endif  // BLUE_SIM_TICK_CLOCK_H_
