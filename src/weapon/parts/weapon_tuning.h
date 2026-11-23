#ifndef BLUE_WEAPON_PARTS_WEAPON_TUNING_H_
#define BLUE_WEAPON_PARTS_WEAPON_TUNING_H_

#include <optional>
#include <string>
#include <string_view>

#include "weapon/firearm.h"
#include "weapon/parts/weapon_stats.h"
#include "weapon/viewmodel.h"

// How a part-built weapon's stats become the game's firearm spec and
// viewmodel feel. One small function, so the mapping is tested in one place.

namespace blue::weapon::parts {

// Zero distances a weapon's sights can be set to with the zero keys: from
// `min` to `max` in `step` steps (metres along the line of sight).
struct ZeroRange {
  float min = 25.0f;   // > 0
  float max = 50.0f;   // >= min
  float step = 25.0f;  // > 0

  friend bool operator==(const ZeroRange&, const ZeroRange&) = default;
};

// The nearest zero of every part-built weapon, and its step (m).
inline constexpr float kMinZeroDistance = 25.0f;
inline constexpr float kZeroStep = 25.0f;

// Returns `distance` clamped to `range`.
[[nodiscard]] float ClampZero(const ZeroRange& range, float distance);
// Returns the zero `steps` steps (+ = further) from `distance`, clamped.
[[nodiscard]] float StepZero(const ZeroRange& range, float distance, int steps);

// Everything the game derives from a build's stats.
struct BuildTuning {
  FirearmSpec spec;
  ViewmodelTuning viewmodel;
  std::optional<float> aim_fov_scale;  // nullopt: the player's default
  // Multiplies the loadout's holster and draw times
  // (WeaponItem::switch_time_scale).
  float switch_time_scale = 1.0f;
  // Shot dispersion (weapon/dispersion.h); 0 = none.
  float accuracy_moa = 0.0f;
  // nullopt: the zero is fixed at viewmodel.zero_distance (no sights).
  std::optional<ZeroRange> zero_range;
};

// Returns the handling multiplier of `ergonomics` against the base's
// `reference`: 1 + (reference - ergonomics) / 100, at least 0.5. Every
// ergonomics point below the reference makes aiming, sway and weapon
// switches 1 % slower (of the reference time), every point above 1 % faster.
[[nodiscard]] float HandlingScale(float ergonomics, float reference);

// Maps `stats` onto the weapon kind's base (PartBuiltBaseFor(), in
// weapon_specs.h), which supplies everything parts do not define:
// - spec: display name `display_name` (must outlive the spec), magazine
//   capacity, fire rate, fire modes (the root's first is the start mode),
//   muzzle velocity, kind, mass = weight and functional from the stats; a
//   zero fire rate, muzzle velocity or weight keeps the base's;
// - recoil: look_kick_pitch and recoil_pitch_velocity scale with
//   recoil_vertical / reference_recoil_vertical, look_kick_yaw with
//   recoil_horizontal / reference_recoil_horizontal;
// - handling (HandlingScale of the ergonomics): aim_time, sway_gain and
//   switch_time_scale;
// - zeroing: viewmodel.muzzle_velocity follows the spec; with a sighting
//   range the zero can be set from kMinZeroDistance (or the range, if
//   nearer) to the range in kZeroStep steps and starts at the base's zero
//   clamped to that; without one it stays at the base's zero;
// - accuracy_moa from the stats.
// The stock Glock 17 preset of the sample catalogue maps exactly onto
// Glock17Spec() / Glock17ViewmodelTuning() (but its name and mass).
[[nodiscard]] BuildTuning TuneFromStats(const WeaponStats& stats,
                                        std::string_view display_name);

// Returns the compact stats line of the weapon readout: weight,
// ergonomics, accuracy, sighting range, recoil and fire rate, e.g.
// "0.91 kg  ergo 91  4.4 MOA  50 m  recoil 302/252  1100 rpm" ("no sights"
// for a range of 0, "- MOA" without a barrel figure).
[[nodiscard]] std::string StatsSummary(const WeaponStats& stats);

// Returns "NOT FUNCTIONAL: missing <slot>, <slot>" for a build with empty
// required slots, an empty string for a functional one.
[[nodiscard]] std::string MissingSummary(const WeaponStats& stats);

}  // namespace blue::weapon::parts

#endif  // BLUE_WEAPON_PARTS_WEAPON_TUNING_H_
