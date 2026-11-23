#ifndef BLUE_WEAPON_WEAPON_GEOMETRY_H_
#define BLUE_WEAPON_WEAPON_GEOMETRY_H_

#include <glm/vec3.hpp>

namespace blue::weapon {

// Calibration of a weapon model: reference points in model units and the
// scale to metres. Model axes: up is +Y and the weapon is upright; the bore
// may point along any horizontal model axis (the M16 fires along +Z, the
// Glock along -X). Nothing assumes a particular bore axis.
//
// The sight line is the line the aimed eye looks along, whatever the sight:
// for a collimated optic (red dot) it is the optic axis, for iron sights the
// line from the top of the rear notch over the top of the front post.
struct WeaponGeometry {
  float scale = 1.0f;      // model units -> metres
  glm::vec3 muzzle{0.0f};  // centre of the bore exit
  // Rear end of the sight line, where the aimed eye looks from: the rear
  // opening of an optic, or the top centre of an iron sight's rear notch.
  glm::vec3 sight_eye_point{0.0f};
  // Unit direction of the sight line (downrange). A collimated dot is an
  // image at infinity along it.
  glm::vec3 sight_axis{0.0f, 0.0f, 1.0f};
  // Centre of an optic's lens (the window the dot is seen in); the lens is a
  // disc of `optic_lens_radius` perpendicular to `sight_axis`. The axis need
  // not pass through the lens centre.
  glm::vec3 optic_lens{0.0f};
  // Model units. 0 means the weapon has no optic: draw no dot.
  float optic_lens_radius = 0.0f;
  glm::vec3 grip{0.0f};  // pistol grip, where the firing hand sits
  // Centre of a scope's eyepiece glass, on the sight line: where the
  // magnified picture is drawn (ScopeOptics::eyepiece_radius around it,
  // perpendicular to the axis). sight_eye_point for weapons without a scope.
  glm::vec3 eyepiece{0.0f};
};

// Returns the calibration of samples/m16.obj, measured from the vertices and
// faces of each material with throwaway scripts (re-measured for the
// re-export without the front sight):
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
// - sight_eye_point (0, 1.354, -1.505), axis +Z: through the centre of the
//   window, 5.8 cm above the bore. The front sight has been removed (no
//   faces above the barrel's y 0.85 at z 4.95 .. 5.55; the file keeps 8
//   unused vertices of its old tips, which the importer drops), so nothing
//   ahead of the hood reaches the window: +Z rays from the rear face are
//   clear at x = 0 for y 1.246 .. 1.468, at the axis height for
//   |x| < 0.16, and over the whole disc of radius 0.10 around the axis. Below
//   the line the nearest geometry is the hood floor and the mount just ahead of
//   it (y 1.245, 8.4 mm below), then the receiver and handguard (y <= 1.056, 23
//   mm below).
// - grip (0, -0.75, -2.0): middle of the Grip vertices behind z = -1
//   (y [-1.52, -0.04], raked back from z -1.55 at the top to -2.9).
[[nodiscard]] const WeaponGeometry& M16Geometry();

// Returns the calibration of samples/glock/glock17.glb in the model space of
// its rest pose (every node translation applied, Pistol_Root's (1, 0.2, 0)
// included; no node is rotated or scaled at rest), measured from the
// vertices of each mesh with a throwaway Node.js script. Bore along -X.
// - scale 0.0842: the Slide spans x [-1.179, 1.031], 2.210 units; a Glock 17
//   slide is 0.186 m long. Cross-checks: overall length 2.37 units = 0.199 m
//   (real 0.202 m), sight radius 0.167 m (real 0.165 m).
// - muzzle (-1.211, 0.626, 0): the Barrel's front face is at x -1.2112; its
//   ring vertices (outer radius 0.084, bore radius 0.060-0.064) centre on
//   y 0.6255, z 0.004 (z rounded to the symmetry plane: 0.3 mm).
// - Iron sights on the Slide (top flat at y 0.759): the rear sight spans
//   x [0.872, 0.913] with two ears at |z| 0.033 .. 0.085 up to y 0.810 and a
//   notch between them with its bottom at y 0.784; the front post spans
//   x [-1.130, -1.043], |z| < 0.023, its top at y 0.819 for x
//   [-1.107, -1.043]. Confidence: high.
// - sight_eye_point (0.913, 0.810, 0): top centre of the notch (level with
//   the ears) on the rear sight's rear face. sight_axis: from there to the
//   middle of the front post's top (-1.075, 0.819, 0), normalised: the
//   conventional picture, post top level with the ears, 0.26 degrees above
//   the slide.
// - No optic: optic_lens_radius 0 (optic_lens is set to the eye point).
// - grip (0.78, -0.35, 0): middle of the Frame's grip (y [-0.726, 0.08],
//   raked back from x [0.38, 1.0] at y -0.2 to [0.51, 1.19] at y -0.6).
[[nodiscard]] const WeaponGeometry& Glock17Geometry();

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_GEOMETRY_H_
