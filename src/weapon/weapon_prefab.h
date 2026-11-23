#ifndef BLUE_WEAPON_WEAPON_PREFAB_H_
#define BLUE_WEAPON_WEAPON_PREFAB_H_

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>

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
inline constexpr std::string_view kEyepieceSocket = "eyepiece";

// Event names in a weapon's fire clip; see AnimatorTuningFromClip().
inline constexpr std::string_view kShotEvent = "shot";
inline constexpr std::string_view kSlideLockEvent = "slide_lock";

// Returns true if `a` and `b` are equal ignoring ASCII case: how weapon code
// matches socket, clip and event names ("Muzzle" is "muzzle").
[[nodiscard]] bool NamesMatch(std::string_view a, std::string_view b);

// Return the index of the first socket / clip named `name` ignoring case,
// or -1.
[[nodiscard]] int FindSocketIgnoringCase(const prefab::Prefab& prefab,
                                         std::string_view name);
[[nodiscard]] int FindClipIgnoringCase(const prefab::Prefab& prefab,
                                       std::string_view name);

// Returns the first event of `clip` named `name` ignoring case, or null.
// Valid until the clip's events change.
[[nodiscard]] const animation::ClipEvent* FindEventIgnoringCase(
    const animation::Clip& clip, std::string_view name);

// Returns the rest-pose prefab-space matrix of every node of `prefab`.
[[nodiscard]] std::vector<glm::mat4> RestMatrices(const prefab::Prefab& prefab);

// Returns rest[node], or the identity for node -1 (the prefab root) and any
// index outside `rest`.
[[nodiscard]] glm::mat4 RestNodeMatrix(std::span<const glm::mat4> rest,
                                       int node);

// Returns the prefab-space matrix of the first socket named `name` (ignoring
// case) at the rest pose `rest` (RestMatrices()), or nullopt without one.
[[nodiscard]] std::optional<glm::mat4> SocketRestMatrix(
    const prefab::Prefab& prefab, std::span<const glm::mat4> rest,
    std::string_view name);

// Returns the calibration of a weapon prefab, taken from its sockets at the
// rest pose (a socket on a node follows that node's rest transform):
// - "muzzle": translation = WeaponGeometry::muzzle.
// - "sight": translation = sight_eye_point; its rotation maps +Z onto
//   sight_axis.
// - "grip": translation = grip.
// - "lens" (optional): translation = optic_lens; the socket's scale along its
//   local X is optic_lens_radius. Without it the weapon has no optic (radius
//   0, optic_lens = sight_eye_point).
// - "eyepiece" (optional, scoped weapons): translation = eyepiece, the
//   centre of the scope's eyepiece glass on its axis. Without it the
//   eyepiece is taken to be at sight_eye_point.
// Socket names match ignoring case. The prefab is in metres, so the
// result's scale is 1. A missing required socket is an error naming it.
[[nodiscard]] core::Result<WeaponGeometry, std::string> GeometryFromSockets(
    const prefab::Prefab& prefab);

// Returns the animator timing of a fire clip from its events (names matched
// ignoring case): "shot" gives fire_time and "slide_lock" lock_time;
// slide_node keeps its default. A missing event is an error naming it.
[[nodiscard]] core::Result<WeaponAnimatorTuning, std::string>
AnimatorTuningFromClip(const animation::Clip& clip);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_PREFAB_H_
