#ifndef BLUE_WEAPON_RIFLE_GEOMETRY_H_
#define BLUE_WEAPON_RIFLE_GEOMETRY_H_

#include <glm/vec3.hpp>

namespace blue::weapon {

// Calibration of a rifle model with a collimated (red dot) optic: reference
// points in model units and the scale to metres. Model axes: the bore points
// along +Z, up is +Y, and the rifle is symmetric about x = 0.
struct RifleGeometry {
  float scale = 1.0f;      // model units -> metres
  glm::vec3 muzzle{0.0f};  // centre of the bore exit
  // Point on the optic axis at the rear opening of the optic: the aimed eye
  // looks through it along `optic_axis`.
  glm::vec3 optic_eye_side{0.0f};
  // Unit direction of the optic axis (downrange). The dot is an image at
  // infinity along it.
  glm::vec3 optic_axis{0.0f, 0.0f, 1.0f};
  // Centre of the lens (the window the dot is seen in); the lens is a disc of
  // `optic_lens_radius` perpendicular to `optic_axis`. The axis need not pass
  // through the lens centre.
  glm::vec3 optic_lens{0.0f};
  float optic_lens_radius = 0.0f;  // model units
  glm::vec3 grip{0.0f};            // pistol grip, where the firing hand sits
};

// Returns the calibration of samples/m16.obj, measured from the vertices and
// faces of each material with throwaway awk scripts:
// - scale 0.0771: the model spans z [-5.921, 8.996], 14.92 units; an
//   M16A2 (1.006 m) with this suppressor is about 1.15 m long.
// - muzzle (0, 0.598, 8.995): the Silencer's largest z (8.9957); y is the
//   centre of its y range [0.308, 0.888], as is the barrel's [0.500, 0.696].
// - The optic is a small open-hood reflex sight (RMR-like) on the receiver's
//   rail (rail top y 1.088). Its hood has a flat rear face at z -1.506, a
//   floor at y 1.239, a roof underside at y 1.468, side walls with inner
//   |x| ~0.15 that curve in near the roof, and a front edge slanting from
//   z -1.22 (floor) to -1.34 (roof). The window is open: the model has no
//   lens face. Confidence: high for the window and axis, medium for the lens
//   depth (anything between the rear face and the front edge is defensible).
// - optic_lens (0, 1.354, -1.27), radius 0.11 (8.5 mm): middle of the window
//   height at the hood's front edge; the radius is limited by the 0.115 half
//   height and the rounded top corners.
// - optic_eye_side (0, 1.44, -1.505), axis +Z: the A2 front sight ahead
//   (z 5.18 .. 5.36) is solid across |x| < 0.09 up to y 1.412 and has two
//   ears at |x| 0.055 .. 0.092 up to y 1.508 with an open notch between. A
//   +Z line at x = 0 clears it only for y >= 1.412, and the window ends at
//   1.468, so the axis runs at y 1.44 through the notch (a co-witness look,
//   6.5 cm above the bore).
// - grip (0, -0.75, -2.0): middle of the Grip vertices behind z = -1
//   (y [-1.52, -0.04], raked back from z -1.55 at the top to -2.9).
[[nodiscard]] const RifleGeometry& M16Geometry();

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_RIFLE_GEOMETRY_H_
