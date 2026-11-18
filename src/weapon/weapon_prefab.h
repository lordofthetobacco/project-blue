#ifndef BLUE_WEAPON_WEAPON_PREFAB_H_
#define BLUE_WEAPON_WEAPON_PREFAB_H_

#include <string>
#include <string_view>

#include "animation/clip.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "weapon/weapon_animator.h"
#include "weapon/weapon_geometry.h"

// Weapon data read from cooked prefabs: the calibration from the prefab's
// sockets and the fire animation's timing from its clip events.

namespace blue::weapon {

// Socket names a weapon prefab carries. Positions are in prefab space
// (metres); see GeometryFromSockets().
inline constexpr std::string_view kMuzzleSocket = "muzzle";
inline constexpr std::string_view kSightSocket = "sight";
inline constexpr std::string_view kGripSocket = "grip";
inline constexpr std::string_view kLensSocket = "lens";

// Event names in a weapon's fire clip; see AnimatorTuningFromClip().
inline constexpr std::string_view kShotEvent = "shot";
inline constexpr std::string_view kSlideLockEvent = "slide_lock";

// Returns the calibration of a weapon prefab, taken from its sockets at the
// rest pose (a socket on a node follows that node's rest transform):
// - "muzzle": translation = WeaponGeometry::muzzle.
// - "sight": translation = sight_eye_point; its rotation maps +Z onto
//   sight_axis.
// - "grip": translation = grip.
// - "lens" (optional): translation = optic_lens; the socket's scale along its
//   local X is optic_lens_radius. Without it the weapon has no optic (radius
//   0, optic_lens = sight_eye_point).
// The prefab is in metres, so the result's scale is 1. A missing required
// socket is an error naming it.
[[nodiscard]] core::Result<WeaponGeometry, std::string> GeometryFromSockets(
    const prefab::Prefab& prefab);

// Returns the animator timing of a fire clip from its events: "shot" gives
// fire_time and "slide_lock" lock_time; slide_node keeps its default. A
// missing event is an error naming it.
[[nodiscard]] core::Result<WeaponAnimatorTuning, std::string>
AnimatorTuningFromClip(const animation::Clip& clip);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_PREFAB_H_
