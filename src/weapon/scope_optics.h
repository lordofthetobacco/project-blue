#ifndef BLUE_WEAPON_SCOPE_OPTICS_H_
#define BLUE_WEAPON_SCOPE_OPTICS_H_

#include <array>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

// The optics of a magnifying rifle scope as the first-person view draws
// them: what a pixel of the eyepiece lens shows, as a function of the
// direction from the eye through it.
//
// Directions are given as "tangents": a direction d seen against the scope's
// frame (unit right r, up u and axis a, downrange) is
// t = (d.r, d.u) / (d.a). The eyepiece maps the world seen along the axis
// at tangent w onto the apparent tangent t = M w (M the magnification), so
// the picture in the lens is a function of direction only, like a red dot:
// it stays on target whatever the eye's position, and the reticle, which
// lies in a focal plane, is fixed in the picture.
//
// On top of that ideal mapping (all pure functions here;
// shaders/scope_lens.slang mirrors them):
// - the field stop: nothing beyond the apparent field |t| > apparent_field;
// - radial distortion and lateral chromatic aberration growing towards the
//   field's edge (DistortApparent());
// - vignetting towards the edge (Vignetting());
// - the eye box (EyeBoxTransmission()): light leaves the eyepiece through a
//   small disc, the exit pupil (radius objective_radius / M), eye_relief
//   behind the eyepiece. The eye's pupil has to overlap it. A ray reaching
//   the eye from apparent direction t crosses the exit pupil's plane at
//   p(t) = e + t (dz + pupil_shadow_depth), e the eye's sideways offset from
//   the axis and dz its distance behind the exit pupil (+ = too far back).
//   The brightness is full while p(t) lies inside the exit pupil and fades
//   out as the eye's pupil around it slides off the exit pupil (zero once
//   the discs are apart): on the axis at the right relief everything is lit;
//   off to one side a dark crescent comes in from that side; too far back or
//   too close the field shrinks to a tunnel; far off all goes black.
//   pupil_shadow_depth stands for the exit pupil's spherical aberration
//   (real exit pupils are not flat), which is what makes a sideways offset
//   at the correct relief show as a crescent rather than an even dimming.

namespace blue::weapon {

// The optical parameters of a scope. Lengths in metres; field and tangents
// are unitless (tan of angles).
struct ScopeOptics {
  // Clear radius of the eyepiece glass: the disc the picture is drawn in.
  float eyepiece_radius = 0.0163f;
  // Clear radius of the objective; the exit pupil's radius is this over the
  // magnification.
  float objective_radius = 0.020f;
  // From the eyepiece glass forward along the axis to the objective, where
  // the scope's view of the world starts.
  float objective_distance = 0.326f;
  // From the eyepiece glass back to the exit pupil, where the eye belongs.
  float eye_relief = 0.096f;
  // tan of the eyepiece's apparent half field: the field stop.
  float apparent_field = 0.18f;
  // Radial distortion at the field's edge: + pincushion, - barrel
  // (t -> t (1 + distortion (|t| / field)^2)).
  float distortion = 0.035f;
  // Lateral chromatic aberration at the field's edge: red is magnified by
  // (1 + this) more, blue by (1 - this) less than green (quadratic in |t|).
  float chromatic_aberration = 0.004f;
  // Share of the brightness lost at the field's edge (quartic in |t|).
  float vignetting = 0.35f;
  // Radius of the viewer's pupil.
  float eye_pupil_radius = 0.003f;
  // See the top of this file: metres of extra relief error per unit
  // apparent tangent.
  float pupil_shadow_depth = 0.008f;
};

// One straight line of a reticle, in the reticle's focal plane: from `from`
// to `to` in units of the apparent field (the field stop is the unit
// circle), `half_width` in apparent tangent (a second-focal-plane reticle:
// its apparent size does not change with the magnification).
struct ReticleLine {
  glm::vec2 from{0.0f};
  glm::vec2 to{0.0f};
  float half_width = 0.0f;
};

// The most lines a reticle has.
inline constexpr int kMaxReticleLines = 8;

// A reticle: up to kMaxReticleLines lines drawn in `color` (linear, before
// exposure; black is an etched line).
struct Reticle {
  std::array<ReticleLine, kMaxReticleLines> lines{};
  int line_count = 0;
  glm::vec3 color{0.0f};
};

// Returns a thin cross through the centre spanning the whole field, its
// lines `half_width` apparent tangent wide (0.0004: about a third of a
// pixel's angle at 1080p with a 70 degree view, so the shader's
// one-pixel floor decides).
[[nodiscard]] Reticle SimpleCrossReticle(float half_width = 0.0004f);

// Returns how far (apparent tangent) `point` (apparent tangent) lies outside
// the nearest line of `reticle` (negative inside a line), or a large value
// without lines.
[[nodiscard]] float ReticleDistance(const Reticle& reticle,
                                    const ScopeOptics& optics, glm::vec2 point);

// Returns the exit pupil's radius at `magnification` (>= 1).
[[nodiscard]] float ExitPupilRadius(const ScopeOptics& optics,
                                    float magnification);

// Where the eye is relative to the scope.
struct EyePlacement {
  // Sideways offset of the eye from the scope's axis, along the scope's
  // right and up, m.
  glm::vec2 lateral{0.0f};
  // How far the eye is behind the exit pupil, m: + too far back, - too
  // close.
  float relief_error = 0.0f;
};

// Returns the eye at world position `eye` against an eyepiece centred at
// `eyepiece` with the scope's unit `right`, `up` and `axis` (downrange).
[[nodiscard]] EyePlacement PlaceEye(const ScopeOptics& optics, glm::vec3 eye,
                                    glm::vec3 eyepiece, glm::vec3 right,
                                    glm::vec3 up, glm::vec3 axis);

// Returns the brightness in [0, 1] of apparent direction `apparent` seen
// by an eye at `placement` at `magnification`: the eye box (see the top of
// this file). Smooth in every input.
[[nodiscard]] float EyeBoxTransmission(const ScopeOptics& optics,
                                       float magnification,
                                       const EyePlacement& placement,
                                       glm::vec2 apparent);

// Returns the largest EyeBoxTransmission() over the apparent field (taken
// where p(t) comes closest to the axis): whether the eye sees anything
// through the scope at all.
[[nodiscard]] float PeakEyeBoxTransmission(const ScopeOptics& optics,
                                           float magnification,
                                           const EyePlacement& placement);

// Returns the undistorted apparent tangent that the eyepiece shows at
// `apparent`: apparent (1 + (distortion + channel_aberration) (|apparent| /
// apparent_field)^2). `channel_aberration` is +chromatic_aberration for red,
// 0 for green and -chromatic_aberration for blue.
[[nodiscard]] glm::vec2 DistortApparent(const ScopeOptics& optics,
                                        glm::vec2 apparent,
                                        float channel_aberration);

// Returns the brightness factor in [1 - vignetting, 1] at `apparent`.
[[nodiscard]] float Vignetting(const ScopeOptics& optics, glm::vec2 apparent);

// Returns tan of the half field the scope's own camera renders at
// `magnification`: the apparent field (with room for the distortion and the
// aberration) divided by the magnification, so the render covers every
// direction the lens can show.
[[nodiscard]] float ScopeCameraTangent(const ScopeOptics& optics,
                                       float magnification);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_SCOPE_OPTICS_H_
