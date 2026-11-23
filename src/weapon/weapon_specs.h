#ifndef BLUE_WEAPON_WEAPON_SPECS_H_
#define BLUE_WEAPON_WEAPON_SPECS_H_

#include <optional>

#include "weapon/firearm.h"
#include "weapon/scope.h"
#include "weapon/viewmodel.h"

namespace blue::weapon {

// Returns the M16A2 firing M855: the FirearmSpec defaults.
[[nodiscard]] FirearmSpec M16Spec();

// Returns the rifle's viewmodel feel (the ViewmodelTuning defaults), zeroed
// for M16Spec()'s muzzle velocity.
[[nodiscard]] ViewmodelTuning M16ViewmodelTuning();

// Returns a Glock 17 firing 9x19 mm 124 gr (8.0 g) FMJ: 17 + 1 rounds, semi
// only, 3 spare magazines. The cyclic limit (1100 rounds per minute, the
// mechanism's rate) only bounds how fast trigger pulls can fire. Drag
// 0.5 * rho * Cd * A / m with air 1.225 kg/m^3, Cd 0.35 (round nose at
// Mach 1.1), 9.01 mm diameter (A 6.38e-5 m^2), 8 g: 1.7e-3 1/m. Weighs
// 0.905 kg loaded (0.625 kg empty).
[[nodiscard]] FirearmSpec Glock17Spec();

// Returns the pistol's viewmodel feel for Glock17Geometry(): a two-handed
// hold, more central and further out than the rifle, arms extended when
// aimed (eye relief 0.45 m), zeroed at 25 m for 375 m/s. The weapon kick
// springs are off (the model's glTF fire animation supplies the visual
// recoil); the camera look kick per shot is larger than the rifle's.
[[nodiscard]] ViewmodelTuning Glock17ViewmodelTuning();

// Seconds between two shots of the FN SPR A3G: working the bolt by hand.
// Bolt action is modelled without animation as semi-automatic fire limited
// to this cycle (SprA3GSpec().rounds_per_minute = 60 / this).
inline constexpr float kSprA3GBoltCycleSeconds = 1.2f;

// Returns an FN SPR A3G (bolt action, 24" barrel) firing 7.62x51 mm M118LR,
// 175 gr (11.34 g) boat tail: 5 + 1 rounds, semi only with a
// kSprA3GBoltCycleSeconds bolt cycle, 4 spare magazines, 790 m/s (3.5 kJ at
// the muzzle; the game's damage is the bullet's mass and velocity). Drag
// 0.5 * rho * Cd * A / m with air 1.225 kg/m^3, Cd 0.28 (boat tail at Mach
// 2.3), 7.82 mm diameter (A 4.80e-5 m^2), 11.34 g: 7.3e-4 1/m. Weighs
// 6.5 kg with the scope and a loaded magazine.
[[nodiscard]] FirearmSpec SprA3GSpec();

// Returns the sniper rifle's viewmodel feel: a heavy rifle, slower to the
// shoulder and swaying more at the hip, the eye 9 cm behind the scope's
// rear rim (9.6 cm behind the eyepiece glass: the scope's eye relief) when
// aimed, zeroed at 100 m for 790 m/s with SprA3GSpec()'s drag (the scope's
// turret re-zeroes it). Fully aimed it holds still (aim_motion_scale 0). A
// stronger kick than the M16's.
[[nodiscard]] ViewmodelTuning SprA3GViewmodelTuning();

// Returns the rifle's Leupold Mark 4 3.5-10x40: 3.5x to 10x in 0.5x steps,
// starting at 3.5x; zero 100 to 300 m in 50 m clicks, starting at 100 m; a
// thin cross. Optics from the model (spr_a3g.pbo, scope node, metres):
// eyepiece glass a dome on the axis (y 0.1822), apex at z 0.2332, clear
// radius 16.3 mm (the rim's inner edge); objective glass apex at
// z -0.0926, clear radius 23.8 mm on the model (a 40 mm objective is used
// for the exit pupil: 11.4 mm at 3.5x, 4 mm at 10x); objective 0.326 m ahead
// of the eyepiece. Eye relief 96 mm (the aimed eye position). Apparent
// half field tan 0.18 (a 3.5-10x's 11-30 ft at 100 yd), which the eyepiece
// rim subtends from the eye at that relief (16.3 / 96 = 0.17), so the
// picture fills the glass. Mild pincushion (3.5 %), 0.4 % lateral colour
// and 35 % vignetting at the edge; eye pupil 3 mm radius.
[[nodiscard]] ScopeSpec SprA3GScope();

// Fully aimed vertical FOV of a pistol as a fraction of the base one: its
// sights are held at arm's length and magnify less than a rifle's cheek weld
// (which keeps the player's default).
inline constexpr float kPistolAimFovScale = 0.9f;

// What a weapon built from parts takes from code rather than from its parts
// (bullet, reload times, spare magazines, poses, eye relief, the base feel),
// per weapon kind, and the stats that feel belongs to:
// weapon/parts/weapon_tuning.h scales the recoil and handling of `viewmodel`
// by the build's stats relative to the reference ones.
struct PartBuiltBase {
  FirearmSpec spec;
  ViewmodelTuning viewmodel;
  std::optional<float> aim_fov_scale;  // nullopt: the player's default
  float reference_ergonomics = 50.0f;
  float reference_recoil_vertical = 100.0f;
  float reference_recoil_horizontal = 100.0f;
};

// Returns the base of part-built weapons of `kind`:
// - pistol: Glock17Spec() and Glock17ViewmodelTuning() (aim zoom
//   kPistolAimFovScale) at the stats of the stock Glock 17 preset of the
//   sample catalogue: ergonomics 90 + 3 (slide) - 2 (barrel) = 91, recoil
//   311 / 260 x (1 - 2.9 %) (barrel), so that build feels exactly as the
//   single-piece Glock 17;
// - rifle: M16Spec() and M16ViewmodelTuning() at the sample catalogue's M16
//   stats (ergonomics 50, recoil 175 / 163).
[[nodiscard]] PartBuiltBase PartBuiltBaseFor(WeaponKind kind);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_SPECS_H_
