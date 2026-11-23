#include "weapon/firearm.h"

#include <algorithm>
#include <cstddef>

namespace blue::weapon {
namespace {

// Reload steps as fractions of the reload time.
constexpr float kMagazineOutFraction = 0.30f;
constexpr float kMagazineInFraction = 0.65f;
constexpr float kBoltReleaseFraction = 0.85f;

constexpr float kSecondsPerMinute = 60.0f;

// Returns `modes`, or kSemi alone when it is empty.
FireModeMask AvailableModes(FireModeMask modes) {
  return modes != 0 ? modes : FireModeBit(FireMode::kSemi);
}

// Returns the mode the selector starts in: `wanted` if available, else the
// first available mode in enum order.
FireMode StartMode(FireMode wanted, FireModeMask modes) {
  const FireModeMask available = AvailableModes(modes);
  if ((available & FireModeBit(wanted)) != 0) {
    return wanted;
  }
  for (int i = 0; i < kFireModeCount; ++i) {
    const auto mode = static_cast<FireMode>(i);
    if ((available & FireModeBit(mode)) != 0) {
      return mode;
    }
  }
  return FireMode::kSemi;
}

// Returns the available mode after `current` in enum order, wrapping round.
FireMode NextMode(FireMode current, FireModeMask modes) {
  const FireModeMask available = AvailableModes(modes);
  for (int step = 1; step <= kFireModeCount; ++step) {
    const auto mode = static_cast<FireMode>((static_cast<int>(current) + step) %
                                            kFireModeCount);
    if ((available & FireModeBit(mode)) != 0) {
      return mode;
    }
  }
  return current;
}

}  // namespace

Firearm::Firearm(const FirearmSpec& spec)
    : spec_(spec), fire_mode_(StartMode(spec.fire_mode, spec.fire_modes)) {
  spec_.magazine_capacity = std::max(spec_.magazine_capacity, 0);
  spec_.spare_magazines = std::max(spec_.spare_magazines, 0);
  magazine_ = spec_.magazine_capacity;
  magazine_inserted_ = spec_.magazine_capacity > 0;
  // Room for every magazine the weapon carries, so reloads never allocate.
  spare_magazines_.reserve(static_cast<size_t>(spec_.spare_magazines) + 1);
  if (spec_.magazine_capacity > 0) {
    spare_magazines_.assign(static_cast<size_t>(spec_.spare_magazines),
                            spec_.magazine_capacity);
  }
}

FirearmEvents Firearm::Update(const FirearmInput& input, float dt) {
  dt = std::max(dt, 0.0f);
  FirearmEvents events;
  if (input.fire_mode_pressed) {
    const FireMode next = NextMode(fire_mode_, spec_.fire_modes);
    events.fire_mode_changed = next != fire_mode_;
    fire_mode_ = next;
  }
  const bool was_reloading = reloading();
  if (input.reload_pressed && TryStartReload()) {
    events.reload_started = true;
  }
  UpdateReload(dt, events);
  const bool reload_blocks = was_reloading || events.reload_started;
  FirearmInput trigger = input;
  trigger.can_fire = input.can_fire && !reload_blocks;
  UpdateTrigger(trigger, dt, events);
  return events;
}

bool Firearm::CancelReload() {
  if (!reloading()) {
    return false;
  }
  // Spares are never empty; a magazine of a zero-capacity weapon is dropped.
  if (incoming_magazine_ > 0) {
    spare_magazines_.push_back(incoming_magazine_);
  }
  incoming_magazine_ = -1;
  reload_duration_ = 0.0f;
  reload_elapsed_ = 0.0f;
  empty_reload_ = false;
  reload_stage_ = 0;
  return true;
}

void Firearm::Refit(const FirearmSpec& spec) {
  (void)CancelReload();
  const int old_capacity = spec_.magazine_capacity;
  // The spare count stays (the constructor reserved room for it), so the
  // spec's own spare_magazines is not applied.
  spec_ = spec;
  spec_.magazine_capacity = std::max(spec_.magazine_capacity, 0);
  spec_.spare_magazines = std::max(spec_.spare_magazines, 0);
  const int capacity = spec_.magazine_capacity;
  if (capacity == 0) {
    magazine_ = 0;
    magazine_inserted_ = false;
  } else if (old_capacity == 0) {
    magazine_ = capacity;
    magazine_inserted_ = true;
  } else {
    magazine_ = std::min(magazine_, capacity);
  }
  if (capacity > 0) {
    for (int& rounds : spare_magazines_) {
      rounds = std::min(rounds, capacity);
    }
  }
  if ((AvailableModes(spec_.fire_modes) & FireModeBit(fire_mode_)) == 0) {
    fire_mode_ = StartMode(spec_.fire_mode, spec_.fire_modes);
  }
}

float Firearm::reload_fraction() const {
  if (!reloading()) {
    return 0.0f;
  }
  return std::clamp(reload_elapsed_ / reload_duration_, 0.0f, 1.0f);
}

int Firearm::rounds_loaded() const {
  return (magazine_inserted_ ? magazine_ : 0) + (chambered_ ? 1 : 0);
}

bool Firearm::TryStartReload() {
  if (reloading() || spare_magazines_.empty() || !spec_.functional ||
      spec_.magazine_capacity == 0) {
    return false;
  }
  if (chambered_ && magazine_inserted_ &&
      magazine_ >= spec_.magazine_capacity) {
    return false;
  }
  // The fullest spare goes into the hand now; the inserted magazine returns
  // to the pouch when it is pulled, so it cannot be picked again.
  const auto fullest =
      std::max_element(spare_magazines_.begin(), spare_magazines_.end());
  incoming_magazine_ = *fullest;
  spare_magazines_.erase(fullest);
  empty_reload_ = !chambered_;
  reload_duration_ = std::max(empty_reload_ ? spec_.empty_reload_seconds
                                            : spec_.tactical_reload_seconds,
                              1.0e-3f);
  reload_elapsed_ = 0.0f;
  reload_stage_ = 0;
  return true;
}

void Firearm::UpdateReload(float dt, FirearmEvents& events) {
  if (!reloading()) {
    return;
  }
  reload_elapsed_ += dt;
  const float fraction = reload_elapsed_ / reload_duration_;
  if (reload_stage_ == 0 && fraction >= kMagazineOutFraction) {
    if (magazine_inserted_ && magazine_ > 0) {
      spare_magazines_.push_back(magazine_);
    }
    magazine_ = 0;
    magazine_inserted_ = false;
    reload_stage_ = 1;
  }
  if (reload_stage_ == 1 && fraction >= kMagazineInFraction) {
    magazine_ = incoming_magazine_;
    magazine_inserted_ = true;
    incoming_magazine_ = -1;
    reload_stage_ = 2;
  }
  if (reload_stage_ == 2 && empty_reload_ && fraction >= kBoltReleaseFraction) {
    // The bolt release strips the top round into the empty chamber.
    if (!chambered_ && magazine_ > 0) {
      --magazine_;
      chambered_ = true;
    }
    bolt_locked_ = false;
    reload_stage_ = 3;
  }
  if (fraction >= 1.0f) {
    reload_duration_ = 0.0f;
    reload_elapsed_ = 0.0f;
    empty_reload_ = false;
    reload_stage_ = 0;
    events.reload_finished = true;
  }
}

void Firearm::UpdateTrigger(const FirearmInput& input, float dt,
                            FirearmEvents& events) {
  if (!input.trigger_held) {
    pull_used_ = false;
  }
  if (!input.trigger_held || !input.can_fire || !spec_.functional) {
    cycle_remaining_ = std::max(cycle_remaining_ - dt, 0.0f);
    return;
  }
  // `shot_time` runs from the start of this update; shots happen whenever the
  // bolt has cycled while the trigger is held.
  float shot_time = cycle_remaining_;
  while (shot_time <= dt && events.shots < kMaxShotsPerUpdate) {
    if (fire_mode_ != FireMode::kAuto && pull_used_) {
      break;
    }
    if (!chambered_) {
      if (!pull_used_) {
        events.dry_fire = true;
        pull_used_ = true;
      }
      break;
    }
    events.shot_age[static_cast<size_t>(events.shots)] = dt - shot_time;
    ++events.shots;
    Discharge();
    pull_used_ = true;
    shot_time += ShotInterval();
  }
  cycle_remaining_ = std::max(shot_time - dt, 0.0f);
}

void Firearm::Discharge() {
  chambered_ = false;
  if (magazine_inserted_ && magazine_ > 0) {
    --magazine_;
    chambered_ = true;
  } else {
    bolt_locked_ = true;
  }
}

float Firearm::ShotInterval() const {
  return spec_.rounds_per_minute > 0.0f
             ? kSecondsPerMinute / spec_.rounds_per_minute
             : 1.0f;
}

}  // namespace blue::weapon
