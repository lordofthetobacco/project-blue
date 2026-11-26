#include "net/prediction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "net/sequence.h"
#include "net/user_command_codec.h"
#include "sim/player_sim.h"
#include "sim/player_state.h"
#include "sim/user_command.h"

namespace blue::net {

Prediction::Prediction(sim::PlayerSim& player, const PredictionConfig& config)
    : player_(player), config_(config) {}

void Prediction::KeepShownAt(glm::vec3 shown) {
  visual_offset_ = shown - player_.feet();
  if (glm::length(visual_offset_) > config_.snap_distance) {
    visual_offset_ = glm::vec3(0.0f);
  }
}

bool Prediction::Reset(const sim::PlayerState& state, uint32_t tick) {
  for (Entry& entry : history_) {
    entry.valid = false;
  }
  newest_tick_ = tick;
  const glm::vec3 shown = player_.feet() + visual_offset_;
  const bool restored = player_.Restore(state);
  KeepShownAt(shown);
  Entry& entry = EntryFor(tick);
  entry = Entry{.tick = tick, .valid = true, .state = player_.Save()};
  entry.command.tick = tick;
  return restored;
}

void Prediction::SkipTo(uint32_t next_tick) {
  if (TickGreater(next_tick, newest_tick_ + 1)) {
    newest_tick_ = next_tick - 1;
  }
}

sim::StepEvents Prediction::Step(const sim::UserCommand& command) {
  sim::UserCommand canonical = Canonical(command);
  canonical.tick = next_tick();
  const sim::StepEvents events = StepPlayer(canonical);
  newest_tick_ = canonical.tick;
  EntryFor(newest_tick_) = Entry{.tick = newest_tick_,
                                 .valid = true,
                                 .command = canonical,
                                 .state = player_.Save()};
  return events;
}

sim::StepEvents Prediction::StepPlayer(const sim::UserCommand& command) {
  // As sim::SimWorld::Step: a dead player's respawn timer runs down before
  // the players step. The respawn itself (where) is the server's; the
  // prediction waits for it at 0.
  (void)player_.CountDownRespawn();
  return player_.Step(command);
}

const Prediction::Entry* Prediction::Find(uint32_t tick) const {
  const Entry& entry = history_[tick % kHistory];
  return entry.valid && entry.tick == tick ? &entry : nullptr;
}

const sim::UserCommand* Prediction::CommandAt(uint32_t tick) const {
  const Entry* entry = Find(tick);
  return entry != nullptr ? &entry->command : nullptr;
}

const sim::PlayerState* Prediction::StateAt(uint32_t tick) const {
  const Entry* entry = Find(tick);
  return entry != nullptr ? &entry->state : nullptr;
}

ReconcileOutcome Prediction::Reconcile(uint32_t server_tick,
                                       const sim::PlayerState& authoritative) {
  if (TickGreater(server_tick, newest_tick_)) {
    (void)Reset(authoritative, server_tick);
    return ReconcileOutcome::kReset;
  }
  const Entry* predicted = Find(server_tick);
  if (predicted == nullptr) {
    return ReconcileOutcome::kIgnored;
  }
  const bool replay = sim::NeedsReplay(predicted->state, authoritative);
  // Everything up to the server's tick is settled.
  for (Entry& entry : history_) {
    if (entry.valid && !TickGreater(entry.tick, server_tick)) {
      entry.valid = false;
    }
  }
  if (!replay) {
    return ReconcileOutcome::kMatched;
  }
  mismatch_predicted_ = predicted->state;
  mismatch_authoritative_ = authoritative;
  mismatch_tick_ = server_tick;

  const glm::vec3 shown = player_.feet() + visual_offset_;
  const glm::vec3 predicted_feet = player_.feet();
  if (!player_.Restore(authoritative)) {
    // Unknown weapons: nothing can be predicted from it; start over.
    (void)Reset(authoritative, server_tick);
    return ReconcileOutcome::kReset;
  }
  ++replays_;
  uint32_t replayed = server_tick;
  for (uint32_t tick = server_tick + 1; !TickGreater(tick, newest_tick_);
       ++tick) {
    Entry& entry = EntryFor(tick);
    if (!entry.valid || entry.tick != tick) {
      break;  // A gap (never after Step()); the player stops before it.
    }
    (void)StepPlayer(entry.command);
    entry.state = player_.Save();
    replayed = tick;
    ++replayed_ticks_;
  }
  newest_tick_ = replayed;
  last_correction_ = glm::length(player_.feet() - predicted_feet);
  max_correction_ = std::max(max_correction_, last_correction_);
  KeepShownAt(shown);
  return ReconcileOutcome::kReplayed;
}

void Prediction::DecayOffset(float seconds) {
  if (!(seconds > 0.0f) || config_.offset_half_life_seconds <= 0.0f) {
    return;
  }
  visual_offset_ *= std::exp2(-seconds / config_.offset_half_life_seconds);
  if (glm::length(visual_offset_) < 1.0e-4f) {
    visual_offset_ = glm::vec3(0.0f);
  }
}

}  // namespace blue::net
