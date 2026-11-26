#ifndef BLUE_NET_CLOCK_SYNC_H_
#define BLUE_NET_CLOCK_SYNC_H_

#include <cstdint>
#include <optional>

namespace blue::net {

// Tunables of ClockSync. Times in seconds, depths in ticks.
struct ClockSyncConfig {
  double tick_seconds = 1.0 / 60.0;
  // The input buffer depth to hold (protocol.md section 9.1: "about 2"),
  // before the measured jitter is added.
  float target_depth = 2.0f;
  // Most jitter (in ticks) added to the target.
  float max_jitter_ticks = 4.0f;
  // No steering while the smoothed depth is this close to the target.
  float dead_band = 0.75f;
  // Time-scale change per tick of depth error, and its limit.
  float gain = 0.01f;
  float max_adjust = 0.05f;
  // Weight of a new depth or round-trip sample in the running averages.
  float smoothing = 0.1f;
  // A hard resync happens beyond this error.
  double resync_seconds = 0.25;
};

// What ClockSync::OnSnapshot asks the client to do.
struct ClockAdjust {
  // Time scale for sim::TickClock::SetTimeScale.
  float time_scale = 1.0f;
  // Set when the client's tick must jump: the tick its next command is for.
  std::optional<uint32_t> resync_tick;
};

// Keeps the client's tick ahead of the server's by half the round trip plus
// a safety margin, so each command arrives just before the server simulates
// its tick (section 9.1 of docs/netcode/protocol.md):
//   - the start tick is the snapshot's server tick plus the round trip plus
//     the target depth (the command then arrives `target` ticks early);
//   - every snapshot reports `input_buffer_depth`; its running average is
//     steered towards the target by a time scale slightly off 1 (with a
//     dead band, so it does not oscillate);
//   - an error beyond `resync_seconds` (or a client tick the server has
//     already passed) resyncs at once. Feedback about commands sent before
//     a resync is ignored.
// Pure arithmetic, no clock of its own. Copyable. Not thread-safe.
class ClockSync {
 public:
  ClockSync() = default;
  explicit ClockSync(const ClockSyncConfig& config) : config_(config) {}
  ClockSync(const ClockSync&) = default;
  ClockSync& operator=(const ClockSync&) = default;

  // Forgets every measurement (a new connection).
  void Reset();

  // Adds one round-trip time measurement (send of a packet to the receipt
  // of its acknowledgement). Non-positive and non-finite samples are
  // ignored.
  void AddRoundTrip(double seconds);

  // Returns the tick the first command should carry when the newest
  // snapshot is of `server_tick`.
  [[nodiscard]] uint32_t StartTick(uint32_t server_tick) const;

  // Starts playing with the first command for `start_tick`: depth reports
  // about earlier ticks (commands sent before) are ignored, as after a
  // resync.
  void Start(uint32_t start_tick);

  // Takes in one snapshot: its server tick and input buffer depth, and the
  // tick of the client's next command. Returns the time scale to run at and
  // possibly a resync.
  ClockAdjust OnSnapshot(uint32_t server_tick, int8_t input_buffer_depth,
                         uint32_t next_client_tick);

  // Smoothed round trip and its mean deviation, in seconds (0 before the
  // first sample).
  [[nodiscard]] double round_trip() const { return round_trip_; }
  [[nodiscard]] double jitter() const { return jitter_; }
  // The depth the clock aims for, in ticks.
  [[nodiscard]] float target_depth() const;
  // Smoothed reported depth, in ticks.
  [[nodiscard]] float smoothed_depth() const { return depth_; }
  [[nodiscard]] float time_scale() const { return time_scale_; }
  [[nodiscard]] int resyncs() const { return resyncs_; }

 private:
  // Round trip in whole ticks, rounded up.
  [[nodiscard]] uint32_t RoundTripTicks() const;
  // Records a resync to `tick`.
  ClockAdjust Resync(uint32_t tick);

  ClockSyncConfig config_;
  double round_trip_ = 0.0;
  double jitter_ = 0.0;
  bool has_round_trip_ = false;
  float depth_ = 0.0f;
  bool has_depth_ = false;
  float time_scale_ = 1.0f;
  // Snapshots older than this tick describe commands sent before the last
  // resync; their depth is ignored.
  std::optional<uint32_t> settle_tick_;
  int resyncs_ = 0;
};

}  // namespace blue::net

#endif  // BLUE_NET_CLOCK_SYNC_H_
