#include "weapon/loadout.h"

#include <algorithm>
#include <cstddef>

namespace blue::weapon {
namespace {

constexpr float kMinSwitchSeconds = 1.0e-3f;

size_t Index(WeaponSlot slot) { return static_cast<size_t>(slot); }

}  // namespace

Loadout::Loadout(const FirearmSpec& pistol, const FirearmSpec& rifle,
                 const LoadoutTuning& tuning)
    : tuning_(tuning), firearms_{Firearm(pistol), Firearm(rifle)} {}

LoadoutEvents Loadout::Update(const LoadoutInput& input, float dt) {
  dt = std::max(dt, 0.0f);
  LoadoutEvents events;
  if (input.select.has_value() && *input.select != selected_) {
    if (selected_ == active_) {
      // Putting the weapon in hand away: its reload cannot go on.
      (void)mutable_firearm(active_).CancelReload();
    }
    selected_ = *input.select;
    events.switch_started = true;
  }

  // A pull made or held while switching must be released before it counts.
  if (!input.firearm.trigger_held) {
    wait_for_release_ = false;
  } else if (switching()) {
    wait_for_release_ = true;
  }
  FirearmInput firearm_input;
  if (switching()) {
    firearm_input.can_fire = false;
  } else {
    firearm_input = input.firearm;
    firearm_input.trigger_held =
        firearm_input.trigger_held && !wait_for_release_;
  }
  events.firearm = mutable_firearm(active_).Update(firearm_input, dt);

  AdvanceSwitch(dt, events);
  return events;
}

float Loadout::raise_fraction() const {
  const float t = std::clamp(raise_, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

const Firearm& Loadout::firearm(WeaponSlot slot) const {
  return firearms_[Index(slot)];
}

Firearm& Loadout::mutable_firearm(WeaponSlot slot) {
  return firearms_[Index(slot)];
}

void Loadout::AdvanceSwitch(float dt, LoadoutEvents& events) {
  if (selected_ != active_) {
    const float lower_rate =
        1.0f / std::max(tuning_.holster_seconds, kMinSwitchSeconds);
    const float lowered = raise_ - dt * lower_rate;
    if (lowered > 0.0f) {
      raise_ = lowered;
      return;
    }
    // At the bottom: swap, then spend the rest of the frame raising.
    dt = -lowered / lower_rate;
    raise_ = 0.0f;
    active_ = selected_;
    events.weapon_changed = true;
  }
  const float raise_rate =
      1.0f / std::max(tuning_.draw_seconds, kMinSwitchSeconds);
  raise_ = std::min(raise_ + dt * raise_rate, 1.0f);
}

}  // namespace blue::weapon
