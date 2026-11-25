#ifndef BLUE_NET_PREDICTION_H_
#define BLUE_NET_PREDICTION_H_

#include <array>
#include <cstdint>

#include <glm/vec3.hpp>

#include "sim/player_sim.h"
#include "sim/player_state.h"
#include "sim/user_command.h"

namespace blue::net {

// Tunables of Prediction.
struct PredictionConfig {
  // Seconds for half of a correction's visual offset to fade.
  float offset_half_life_seconds = 0.1f;
  // Corrections larger than this (m) snap instead of fading.
  float snap_distance = 1.5f;
};

// What one Prediction::Reconcile did.
enum class ReconcileOutcome : uint8_t {
  kMatched,   // The prediction for the tick was right.
  kReplayed,  // Restored the authoritative state and replayed.
  kReset,     // The tick is newer than any prediction: restored, no replay.
  kIgnored,   // The tick is not in the history (older, or handled).
};

// Client-side prediction of the local player: steps a borrowed
// sim::PlayerSim with the canonical (wire-quantised) command of every tick
// and keeps the last kHistory commands and resulting states by tick. A
// snapshot's own state for server tick S is compared with the prediction
// for S (sim::NeedsReplay); on a mismatch the player is restored to it and
// the commands of S+1 .. newest are stepped again. The shown position moves
// by a correction gradually: visual_offset() is the vector from the
// corrected to the previously shown position, fading with a half-life, and
// snapping to zero beyond `snap_distance`.
//
// Borrows `player`, which must outlive it. Fixed size, no allocation. Not
// thread-safe. Neither copyable nor movable (it drives one player).
class Prediction {
 public:
  static constexpr int kHistory = 128;

  Prediction(sim::PlayerSim& player, const PredictionConfig& config);
  Prediction(const Prediction&) = delete;
  Prediction& operator=(const Prediction&) = delete;

  // Restores the player to `state`, taken as the state at the end of
  // `tick`, and forgets the history: the next Step() is for tick + 1.
  // Returns false if the state does not apply (sim::PlayerSim::Restore).
  bool Reset(const sim::PlayerState& state, uint32_t tick);

  // Makes `next_tick` the tick the next Step() is for, without predicting
  // the ticks before it (the player keeps its state; snapshots of the
  // skipped ticks find no prediction and are ignored). For starting play
  // ahead of the newest snapshot and for clock resyncs. Does nothing unless
  // `next_tick` is after next_tick().
  void SkipTo(uint32_t next_tick);

  // Predicts tick next_tick() under `command` (canonicalised, its tick
  // replaced by next_tick()) and stores both. Returns what the step did.
  sim::StepEvents Step(const sim::UserCommand& command);

  // Compares the authoritative state at the end of `server_tick` with the
  // prediction for it; see the class comment. History up to and including
  // `server_tick` is dropped. A tick newer than every prediction resets to
  // it.
  ReconcileOutcome Reconcile(uint32_t server_tick,
                             const sim::PlayerState& authoritative);

  // Returns the canonical command stored for `tick`, or null.
  [[nodiscard]] const sim::UserCommand* CommandAt(uint32_t tick) const;
  // Returns the predicted state stored for `tick`, or null.
  [[nodiscard]] const sim::PlayerState* StateAt(uint32_t tick) const;

  // Fades the visual offset by `seconds` of frame time.
  void DecayOffset(float seconds);
  // Added to the player's simulated feet, gives the shown position.
  [[nodiscard]] glm::vec3 visual_offset() const { return visual_offset_; }

  // The tick the next Step() is for.
  [[nodiscard]] uint32_t next_tick() const { return newest_tick_ + 1; }
  // The tick of the newest prediction (or of the last Reset()).
  [[nodiscard]] uint32_t newest_tick() const { return newest_tick_; }
  // Reconciliations that restored and replayed, in total.
  [[nodiscard]] int replays() const { return replays_; }
  // Ticks stepped again by those replays, in total.
  [[nodiscard]] int replayed_ticks() const { return replayed_ticks_; }
  // The largest correction a replay made to the feet at the newest tick
  // (m), and the latest one.
  [[nodiscard]] float max_correction() const { return max_correction_; }
  [[nodiscard]] float last_correction() const { return last_correction_; }
  // The states of the last mismatch (prediction and authority for the
  // same tick), for diagnostics; valid after the first replay.
  [[nodiscard]] const sim::PlayerState& last_mismatch_predicted() const {
    return mismatch_predicted_;
  }
  [[nodiscard]] const sim::PlayerState& last_mismatch_authoritative() const {
    return mismatch_authoritative_;
  }
  [[nodiscard]] uint32_t last_mismatch_tick() const { return mismatch_tick_; }

 private:
  struct Entry {
    uint32_t tick = 0;
    bool valid = false;
    sim::UserCommand command;
    sim::PlayerState state;
  };

  [[nodiscard]] Entry& EntryFor(uint32_t tick) {
    return history_[tick % kHistory];
  }
  [[nodiscard]] const Entry* Find(uint32_t tick) const;
  // Steps the player one tick as the server does (respawn countdown, then
  // sim::PlayerSim::Step).
  sim::StepEvents StepPlayer(const sim::UserCommand& command);
  // Sets the visual offset so the shown position stays at `shown`.
  void KeepShownAt(glm::vec3 shown);

  sim::PlayerSim& player_;
  PredictionConfig config_;
  std::array<Entry, kHistory> history_{};
  uint32_t newest_tick_ = 0;
  glm::vec3 visual_offset_{0.0f};
  int replays_ = 0;
  int replayed_ticks_ = 0;
  float max_correction_ = 0.0f;
  float last_correction_ = 0.0f;
  sim::PlayerState mismatch_predicted_;
  sim::PlayerState mismatch_authoritative_;
  uint32_t mismatch_tick_ = 0;
};

}  // namespace blue::net

#endif  // BLUE_NET_PREDICTION_H_
