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

}  // namespace

Firearm::Firearm(const FirearmSpec& spec)
    : spec_(spec),
      fire_mode_(spec.select_fire ? spec.fire_mode : FireMode::kSemi) {
  spec_.magazine_capacity = std::max(spec_.magazine_capacity, 0);
  spec_.spare_magazines = std::max(spec_.spare_magazines, 0);
  magazine_ = spec_.magazine_capacity;
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
  if (input.fire_mode_pressed && spec_.select_fire) {
    fire_mode_ =
        fire_mode_ == FireMode::kSemi ? FireMode::kAuto : FireMode::kSemi;
    events.fire_mode_changed = true;
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
  if (reloading() || spare_magazines_.empty()) {
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
  if (!input.trigger_held || !input.can_fire) {
    cycle_remaining_ = std::max(cycle_remaining_ - dt, 0.0f);
    return;
  }
  // `shot_time` runs from the start of this update; shots happen whenever the
  // bolt has cycled while the trigger is held.
  float shot_time = cycle_remaining_;
  while (shot_time <= dt && events.shots < kMaxShotsPerUpdate) {
    if (fire_mode_ == FireMode::kSemi && pull_used_) {
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
