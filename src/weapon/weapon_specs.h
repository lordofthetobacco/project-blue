#ifndef BLUE_WEAPON_WEAPON_SPECS_H_
#define BLUE_WEAPON_WEAPON_SPECS_H_

#include "weapon/firearm.h"
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
// Mach 1.1), 9.01 mm diameter (A 6.38e-5 m^2), 8 g: 1.7e-3 1/m.
[[nodiscard]] FirearmSpec Glock17Spec();

// Returns the pistol's viewmodel feel for Glock17Geometry(): a two-handed
// hold, more central and further out than the rifle, arms extended when
// aimed (eye relief 0.45 m), zeroed at 25 m for 375 m/s. The weapon kick
// springs are off (the model's glTF fire animation supplies the visual
// recoil); the camera look kick per shot is larger than the rifle's.
[[nodiscard]] ViewmodelTuning Glock17ViewmodelTuning();

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_WEAPON_SPECS_H_
