#ifndef BLUE_WEAPON_LOADOUT_H_
#define BLUE_WEAPON_LOADOUT_H_

#include <array>
#include <optional>

#include "weapon/firearm.h"

namespace blue::weapon {

// The carried weapons, one per slot. Default keys: 2 pistol, 3 rifle.
enum class WeaponSlot { kPistol, kRifle };

inline constexpr int kWeaponSlotCount = 2;

// Timing of a weapon switch.
struct LoadoutTuning {
  float holster_seconds = 0.35f;  // s to lower the weapon out of view
  float draw_seconds = 0.45f;     // s to raise the next one until ready
};

// One frame of player intent.
struct LoadoutInput {
  std::optional<WeaponSlot> select;  // edge: the slot whose key was pressed
  FirearmInput firearm;              // for the weapon in hand
};

// What happened during one Loadout::Update.
struct LoadoutEvents {
  FirearmEvents firearm;  // of the weapon in hand only
  // A selection made this frame changed selected(): the weapon in hand
  // starts (or turns back to) lowering, or turns back to rising.
  bool switch_started = false;
  // active() changed this frame: draw the other model from now on (and
  // Viewmodel::Reset() the one coming into the hands).
  bool weapon_changed = false;
};

// Weapon switching between the carried firearms. Owns one Firearm per slot
// and starts with the rifle ready in the hands.
//
// Selecting the weapon already in hand (and not being put away) does
// nothing. Selecting another lowers the one in hand over `holster_seconds`,
// swaps the model (`weapon_changed`) and raises the new one over
// `draw_seconds`. raise_fraction() is eased, continuous and frame-rate
// independent; a long frame carries its leftover time from the lowering into
// the raising and still reports the swap. Re-selecting the weapon being put
// away turns it back up from where it is; selecting another during the raise
// lowers it again from where it is.
//
// Until the weapon is fully raised it cannot fire, dry fire, reload or change
// fire mode, and a trigger held through the switch must be released before
// the weapon fires (semi or auto). Starting a switch cancels a reload in
// progress (Firearm::CancelReload). Only the firearm in hand is updated: the
// other keeps its exact state.
//
// Deterministic, no allocation. Copyable. Not thread-safe.
class Loadout {
 public:
  Loadout(const FirearmSpec& pistol, const FirearmSpec& rifle,
          const LoadoutTuning& tuning = {});
  Loadout(const Loadout&) = default;
  Loadout& operator=(const Loadout&) = default;

  // Advances by `dt` seconds (negative counts as 0) under `input`.
  LoadoutEvents Update(const LoadoutInput& input, float dt);

  // The weapon whose model is in the hands right now.
  [[nodiscard]] WeaponSlot active() const { return active_; }
  // Where a switch in progress is heading; active() when none is.
  [[nodiscard]] WeaponSlot selected() const { return selected_; }
  [[nodiscard]] bool switching() const {
    return selected_ != active_ || raise_ < 1.0f;
  }
  // 0 lowered out of view .. 1 ready, eased (zero slope at both ends).
  [[nodiscard]] float raise_fraction() const;

  [[nodiscard]] const Firearm& firearm(WeaponSlot slot) const;
  [[nodiscard]] const Firearm& active_firearm() const {
    return firearm(active_);
  }
  [[nodiscard]] const LoadoutTuning& tuning() const { return tuning_; }

 private:
  Firearm& mutable_firearm(WeaponSlot slot);
  // Moves raise_ towards its goal over `dt`, swapping weapons at the bottom.
  void AdvanceSwitch(float dt, LoadoutEvents& events);

  LoadoutTuning tuning_;
  std::array<Firearm, kWeaponSlotCount> firearms_;  // indexed by WeaponSlot
  WeaponSlot active_ = WeaponSlot::kRifle;
  WeaponSlot selected_ = WeaponSlot::kRifle;
  float raise_ = 1.0f;  // linear switch progress in [0, 1]
  // The trigger was held at some point of a switch and has not been
  // released since: ignored until it is.
  bool wait_for_release_ = false;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_LOADOUT_H_
