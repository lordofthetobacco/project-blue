#ifndef BLUE_WEAPON_LOADOUT_H_
#define BLUE_WEAPON_LOADOUT_H_

#include <array>
#include <cstddef>
#include <optional>
#include <span>

#include "weapon/firearm.h"
#include "weapon/weapon_item.h"

namespace blue::weapon {

// The carried weapons, one per slot. Default keys: 2 pistol, 3 rifle.
enum class WeaponSlot { kPistol, kRifle };

inline constexpr int kWeaponSlotCount = 2;

// Returns the slot a weapon of `kind` occupies.
[[nodiscard]] WeaponSlot SlotFor(WeaponKind kind);

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
  // starts (or turns back to) lowering, or turns back to rising, or (empty
  // hands) the selected weapon starts rising.
  bool switch_started = false;
  // active() changed this frame: draw the other model from now on (and
  // Viewmodel::Reset() the one coming into the hands).
  bool weapon_changed = false;
};

// What Loadout::PickUp did.
struct PickUpResult {
  // The weapon that occupied the slot the picked-up one went into (a swap),
  // its reload cancelled; the caller drops it into the world.
  std::optional<WeaponItem> replaced;
  // active() changed: like LoadoutEvents::weapon_changed.
  bool weapon_changed = false;
};

// The carried weapons and switching between them. A slot holds at most one
// weapon (its definition id and firearm), of the kind that decides the slot
// (SlotFor() of its firearm's spec); any slot may be empty and so may the
// hands (no model, no fire, no reload, no aim zoom). The loadout only moves
// definition ids around; two weapons of the same kind but different
// definitions simply replace each other in that kind's slot.
//
// Selecting the weapon already in hand (and not being put away), or an empty
// slot, does nothing. Selecting another lowers the one in hand over
// `holster_seconds`, swaps the model (`weapon_changed`) and raises the new
// one over `draw_seconds` (each times the moving weapon's
// WeaponItem::switch_time_scale); from empty hands the new one comes up at once
// from the bottom. raise_fraction() is eased, continuous and frame-rate
// independent; a long frame carries its leftover time from the lowering into
// the raising and still reports the swap. Re-selecting the weapon being put
// away turns it back up from where it is; selecting another during the raise
// lowers it again from where it is.
//
// Until the weapon is fully raised it cannot fire, dry fire, reload or change
// fire mode, and a trigger held through the switch must be released before
// the weapon fires (semi or auto). Starting a switch cancels a reload in
// progress (Firearm::CancelReload). Only the firearm in hand is updated: the
// others keep their exact state.
//
// Drop() throws the weapon in hand away at once and PickUp() takes one into
// the slot of its kind; see there. A firearm leaves and enters the loadout
// with its complete state and definition; nothing is copied or lost on the
// way.
//
// Deterministic, no allocation. Copyable. Not thread-safe.
class Loadout {
 public:
  // Empty slots and empty hands.
  explicit Loadout(const LoadoutTuning& tuning = {});
  // Each of `weapons` in the slot of its kind (a later one replaces an
  // earlier one of the same kind), the last one ready in the hands; empty
  // hands if `weapons` is empty.
  explicit Loadout(std::span<const WeaponItem> weapons,
                   const LoadoutTuning& tuning = {});
  Loadout(const Loadout&) = default;
  Loadout& operator=(const Loadout&) = default;

  // Advances by `dt` seconds (negative counts as 0) under `input`.
  LoadoutEvents Update(const LoadoutInput& input, float dt);

  // Removes the weapon in hand from its slot at once (it is thrown: no
  // holster time) and returns it, a reload in progress cancelled first. Its
  // whole state goes with it: rounds, chamber, bolt, fire mode and, until
  // the player carries ammunition of their own, its spare magazines.
  //
  // Returns nullopt and changes nothing with empty hands and while the
  // weapon in hand is being lowered for a switch (selected() != active()):
  // only a weapon that is up or coming up can be thrown. Afterwards the
  // first other carried weapon (rifle before pistol) is drawn from the
  // bottom with the normal draw timing, or the hands stay empty. Either way
  // active() changed (handle it like LoadoutEvents::weapon_changed).
  std::optional<WeaponItem> Drop();

  // Puts `weapon` into the slot of its kind and selects it. A weapon in
  // that slot is returned in PickUpResult::replaced. If that slot is the one
  // in hand (or the hands are empty), the new weapon replaces the model at
  // once and is raised from the bottom; otherwise the weapon in hand is
  // lowered first as in a normal switch (its reload cancelled).
  // Moving `weapon` in allocates nothing.
  PickUpResult PickUp(WeaponItem weapon);

  // Replaces the weapon in hand by `weapon` (the same weapon after its parts
  // changed), keeping the switch and raise state. Returns false and changes
  // nothing with empty hands or when `weapon` belongs in another slot.
  bool ReplaceActive(WeaponItem weapon);

  // Sets the scope magnification remembered by the weapon in hand
  // (WeaponItem::scope_magnification). Returns false and changes nothing
  // with empty hands.
  bool SetScopeMagnification(float magnification);
  // Sets the scope zero remembered by the weapon in hand
  // (WeaponItem::scope_zero). Returns false and changes nothing with empty
  // hands.
  bool SetScopeZero(float distance);

  // The weapon whose model is in the hands right now; nullopt for empty
  // hands.
  [[nodiscard]] std::optional<WeaponSlot> active() const { return active_; }
  // Where a switch in progress is heading; active() when none is.
  [[nodiscard]] std::optional<WeaponSlot> selected() const { return selected_; }
  [[nodiscard]] bool hands_empty() const { return !active_.has_value(); }
  [[nodiscard]] bool switching() const {
    return selected_ != active_ || (active_.has_value() && raise_ < 1.0f);
  }
  // 0 lowered out of view .. 1 ready, eased (zero slope at both ends); 0
  // with empty hands.
  [[nodiscard]] float raise_fraction() const;

  // The weapon in `slot`, or null when the slot is empty. Valid until the
  // next non-const call.
  [[nodiscard]] const WeaponItem* item(WeaponSlot slot) const;
  // The weapon in hand, or null with empty hands.
  [[nodiscard]] const WeaponItem* active_item() const;
  // The firearm of item(slot) / active_item(), or null.
  [[nodiscard]] const Firearm* firearm(WeaponSlot slot) const;
  [[nodiscard]] const Firearm* active_firearm() const;
  [[nodiscard]] const LoadoutTuning& tuning() const { return tuning_; }

 private:
  [[nodiscard]] std::optional<WeaponItem>& slot(WeaponSlot slot) {
    return slots_[static_cast<size_t>(slot)];
  }
  // Starts lowering the weapon in hand towards `target` (or raising it
  // back when `target` is the one in hand). Returns whether selected()
  // changed.
  bool Select(WeaponSlot target);
  // Moves raise_ towards its goal over `dt`, swapping weapons at the bottom.
  void AdvanceSwitch(float dt, LoadoutEvents& events);

  LoadoutTuning tuning_;
  // Indexed by WeaponSlot; each holds a weapon of the slot's kind.
  std::array<std::optional<WeaponItem>, kWeaponSlotCount> slots_;
  // Both name occupied slots or are nullopt; selected_ is nullopt only
  // together with active_ (empty hands, nothing to draw).
  std::optional<WeaponSlot> active_;
  std::optional<WeaponSlot> selected_;
  float raise_ = 0.0f;  // linear switch progress in [0, 1]; 0 empty-handed
  // The trigger was held at some point of a switch and has not been
  // released since: ignored until it is.
  bool wait_for_release_ = false;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_LOADOUT_H_
