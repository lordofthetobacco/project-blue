#include "weapon/loadout.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace blue::weapon {
namespace {

constexpr float kMinSwitchSeconds = 1.0e-3f;

// Indexed by WeaponKind.
constexpr std::array<WeaponSlot, kWeaponKindCount> kSlotForKind{
    WeaponSlot::kPistol,  // kPistol
    WeaponSlot::kRifle,   // kRifle
};

// Which carried weapon comes up after the one in hand was dropped: the
// first occupied slot in this order.
constexpr std::array<WeaponSlot, kWeaponSlotCount> kDrawAfterDrop{
    WeaponSlot::kRifle,
    WeaponSlot::kPistol,
};

}  // namespace

WeaponSlot SlotFor(WeaponKind kind) {
  return kSlotForKind[static_cast<size_t>(kind)];
}

Loadout::Loadout(const LoadoutTuning& tuning) : tuning_(tuning) {}

Loadout::Loadout(std::span<const WeaponItem> weapons,
                 const LoadoutTuning& tuning)
    : tuning_(tuning) {
  if (weapons.empty()) {
    return;
  }
  for (const WeaponItem& weapon : weapons) {
    slot(SlotFor(weapon.firearm.spec().kind)) = weapon;
  }
  active_ = SlotFor(weapons.back().firearm.spec().kind);
  selected_ = active_;
  raise_ = 1.0f;
}

LoadoutEvents Loadout::Update(const LoadoutInput& input, float dt) {
  dt = std::max(dt, 0.0f);
  LoadoutEvents events;
  if (input.select.has_value() && slot(*input.select).has_value()) {
    events.switch_started = Select(*input.select);
  }

  // A pull made or held while switching must be released before it counts.
  if (!input.firearm.trigger_held) {
    wait_for_release_ = false;
  } else if (switching()) {
    wait_for_release_ = true;
  }
  if (active_.has_value()) {
    FirearmInput firearm_input;
    if (switching()) {
      firearm_input.can_fire = false;
    } else {
      firearm_input = input.firearm;
      firearm_input.trigger_held =
          firearm_input.trigger_held && !wait_for_release_;
    }
    events.firearm = slot(*active_)->firearm.Update(firearm_input, dt);
  }

  AdvanceSwitch(dt, events);
  return events;
}

std::optional<WeaponItem> Loadout::Drop() {
  if (!active_.has_value() || selected_ != active_) {
    return std::nullopt;
  }
  std::optional<WeaponItem>& held = slot(*active_);
  (void)held->firearm.CancelReload();
  std::optional<WeaponItem> thrown = std::move(held);
  held.reset();
  active_.reset();
  selected_.reset();
  raise_ = 0.0f;
  for (const WeaponSlot next : kDrawAfterDrop) {
    if (slot(next).has_value()) {
      active_ = next;
      selected_ = next;
      break;
    }
  }
  return thrown;
}

PickUpResult Loadout::PickUp(WeaponItem weapon) {
  const WeaponSlot target = SlotFor(weapon.firearm.spec().kind);
  PickUpResult result;
  std::optional<WeaponItem>& destination = slot(target);
  if (destination.has_value()) {
    (void)destination->firearm.CancelReload();
    result.replaced = std::move(destination);
  }
  destination = std::move(weapon);
  if (!active_.has_value() || active_ == target) {
    // Nothing else to put away: the new weapon comes up from the bottom.
    active_ = target;
    selected_ = target;
    raise_ = 0.0f;
    result.weapon_changed = true;
  } else {
    (void)Select(target);
  }
  return result;
}

float Loadout::raise_fraction() const {
  const float t = std::clamp(raise_, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

const WeaponItem* Loadout::item(WeaponSlot slot) const {
  const std::optional<WeaponItem>& held = slots_[static_cast<size_t>(slot)];
  return held.has_value() ? &*held : nullptr;
}

bool Loadout::SetScopeMagnification(float magnification) {
  if (!active_.has_value()) {
    return false;
  }
  slot(*active_)->scope_magnification = magnification;
  return true;
}

bool Loadout::ReplaceActive(WeaponItem weapon) {
  if (!active_.has_value() || SlotFor(weapon.firearm.spec().kind) != *active_) {
    return false;
  }
  *slot(*active_) = std::move(weapon);
  return true;
}

bool Loadout::SetScopeZero(float distance) {
  if (!active_.has_value()) {
    return false;
  }
  slot(*active_)->scope_zero = distance;
  return true;
}

const WeaponItem* Loadout::active_item() const {
  return active_.has_value() ? item(*active_) : nullptr;
}

const Firearm* Loadout::firearm(WeaponSlot slot) const {
  const WeaponItem* held = item(slot);
  return held != nullptr ? &held->firearm : nullptr;
}

const Firearm* Loadout::active_firearm() const {
  const WeaponItem* held = active_item();
  return held != nullptr ? &held->firearm : nullptr;
}

bool Loadout::Select(WeaponSlot target) {
  if (selected_ == target) {
    return false;
  }
  if (active_.has_value() && selected_ == active_) {
    // Putting the weapon in hand away: its reload cannot go on.
    (void)slot(*active_)->firearm.CancelReload();
  }
  selected_ = target;
  return true;
}

void Loadout::AdvanceSwitch(float dt, LoadoutEvents& events) {
  if (selected_ != active_) {
    if (active_.has_value()) {
      const float lower_rate =
          1.0f /
          std::max(tuning_.holster_seconds * slot(*active_)->switch_time_scale,
                   kMinSwitchSeconds);
      const float lowered = raise_ - dt * lower_rate;
      if (lowered > 0.0f) {
        raise_ = lowered;
        return;
      }
      // At the bottom: swap, then spend the rest of the frame raising.
      dt = -lowered / lower_rate;
    }
    raise_ = 0.0f;
    active_ = selected_;
    events.weapon_changed = true;
  }
  if (!active_.has_value()) {
    raise_ = 0.0f;
    return;
  }
  const float raise_rate =
      1.0f / std::max(tuning_.draw_seconds * slot(*active_)->switch_time_scale,
                      kMinSwitchSeconds);
  raise_ = std::min(raise_ + dt * raise_rate, 1.0f);
}

}  // namespace blue::weapon
