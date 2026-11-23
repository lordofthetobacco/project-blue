#ifndef BLUE_WEAPON_SCOPE_H_
#define BLUE_WEAPON_SCOPE_H_

// A magnifying weapon optic: its zoom range, its zero range, its optics and
// reticle, and the zoom the player has chosen (stepped by the mouse wheel,
// smoothed).
//
// The scope is used like the red dot: aiming brings the modelled scope in
// front of the eye with the normal viewmodel motion, and the magnified world
// is drawn inside the eyepiece lens (a second, narrow view along the scope's
// own axis; weapon/scope_optics.h for what each pixel of the lens shows).
// The rest of the screen stays the normal view.

#include "weapon/scope_optics.h"

namespace blue::weapon {

// The optic of a weapon type. Magnifications are true magnifications of the
// unaimed view (1 = none). Zero distances are metres along the line of
// sight.
struct ScopeSpec {
  float min_magnification = 1.0f;    // >= 1
  float max_magnification = 1.0f;    // >= min_magnification
  float step = 0.5f;                 // per wheel notch, > 0
  float start_magnification = 1.0f;  // a fresh weapon's, within the range
  // s for most (~95 %) of a change of magnification.
  float zoom_time = 0.12f;
  // The elevation turret: the zero can be set from min_zero to max_zero in
  // zero_step steps; a fresh weapon starts at start_zero.
  float min_zero = 100.0f;    // > 0
  float max_zero = 100.0f;    // >= min_zero
  float zero_step = 50.0f;    // > 0
  float start_zero = 100.0f;  // within the range
  ScopeOptics optics;
  Reticle reticle = SimpleCrossReticle();
};

// Returns `magnification` clamped to the range of `spec`.
[[nodiscard]] float ClampMagnification(const ScopeSpec& spec,
                                       float magnification);

// Returns `distance` clamped to the zero range of `spec`.
[[nodiscard]] float ClampZero(const ScopeSpec& spec, float distance);

// Returns the zero `steps` turret clicks (zero_step each, + = further) from
// `distance`, clamped to the range of `spec`.
[[nodiscard]] float StepZero(const ScopeSpec& spec, float distance, int steps);

// The magnification the player chose for a scope and the one shown,
// following it smoothly (exponentially in log space, so every step feels the
// same). The wheel moves the chosen value by `step` per whole notch (wheel up
// = zoom in), clamped to the range; fractional notches of high-resolution
// wheels add up until they make a whole one.
//
// Deterministic, frame-rate independent, no allocation. Copyable. Not
// thread-safe.
class ScopeZoom {
 public:
  // Starts settled at `magnification`, clamped to the range of `spec`.
  ScopeZoom(const ScopeSpec& spec, float magnification);

  // Adds `notches` of wheel motion (+ = away from the user = zoom in).
  // Returns whether the chosen magnification changed.
  bool AddWheel(float notches);
  // Moves the shown magnification towards the chosen one by `dt` seconds
  // (negative counts as 0).
  void Update(float dt);

  // The chosen magnification: what the weapon remembers.
  [[nodiscard]] float target() const { return target_; }
  // The magnification to render with this frame.
  [[nodiscard]] float current() const { return current_; }
  [[nodiscard]] const ScopeSpec& spec() const { return spec_; }

 private:
  ScopeSpec spec_;
  float target_ = 1.0f;
  float current_ = 1.0f;
  float pending_notches_ = 0.0f;  // fraction of a notch not yet applied
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_SCOPE_H_
