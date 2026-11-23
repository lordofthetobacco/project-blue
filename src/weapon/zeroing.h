#ifndef BLUE_WEAPON_ZEROING_H_
#define BLUE_WEAPON_ZEROING_H_

// Zeroing a sight: the elevation of the bore over the line of sight that
// makes the bullet cross the line at a chosen distance, from the real
// geometry (bore below and ahead of the eye) and the real ballistics
// (gravity and quadratic drag, flown with ProjectileSystem's integrator).

namespace blue::weapon {

// A shot along a level line of sight, in the sight line's vertical plane.
// Distances in metres from the eye (the line of sight's origin).
struct ZeroSetup {
  float muzzle_below = 0.0f;     // m, bore exit below the line of sight
  float muzzle_ahead = 0.0f;     // m, bore exit ahead of the eye
  float muzzle_velocity = 0.0f;  // m/s
  float drag = 0.0f;             // 1/m, as FirearmSpec::bullet_drag
  float gravity = 9.81f;         // m/s^2
};

// Integration step of the zero computation: ProjectileSystem's default
// longest substep.
inline constexpr float kZeroStepSeconds = 1.0f / 240.0f;

// Returns the height (m, + above) of a bullet fired at `angle` radians above
// the level line of sight when it is `distance` m from the eye, flown with
// IntegrateBallisticStep() in kZeroStepSeconds steps (linear between two
// steps). Without a positive muzzle velocity, or once the bullet would take
// more than 10 s, returns -infinity.
[[nodiscard]] float HeightAtDistance(const ZeroSetup& setup, float angle,
                                     float distance);

// Returns the angle (radians, + = bore pointing above the line of sight) at
// which the bullet crosses the line of sight at `distance` m from the eye,
// on the flat trajectory (the lower of the two angles). Without drag it is the
// exact drag-free solution; with drag it is solved on HeightAtDistance() to
// well under a millimetre. 0 for a non-positive distance or speed.
[[nodiscard]] float ZeroAngle(const ZeroSetup& setup, float distance);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_ZEROING_H_
