#include "weapon/weapon_geometry.h"

namespace blue::weapon {

const WeaponGeometry& M16Geometry() {
  static constexpr WeaponGeometry kGeometry{
      .scale = 0.0771f,
      .muzzle = {0.0f, 0.598f, 8.995f},
      .sight_eye_point = {0.0f, 1.354f, -1.505f},
      .sight_axis = {0.0f, 0.0f, 1.0f},
      .optic_lens = {0.0f, 1.354f, -1.27f},
      .optic_lens_radius = 0.11f,
      .grip = {0.0f, -0.75f, -2.0f},
  };
  return kGeometry;
}

const WeaponGeometry& Glock17Geometry() {
  static constexpr WeaponGeometry kGeometry{
      .scale = 0.0842f,
      .muzzle = {-1.211f, 0.626f, 0.0f},
      .sight_eye_point = {0.913f, 0.810f, 0.0f},
      .sight_axis = {-0.999990f, 0.004527f, 0.0f},
      .optic_lens = {0.913f, 0.810f, 0.0f},
      .optic_lens_radius = 0.0f,
      .grip = {0.78f, -0.35f, 0.0f},
  };
  return kGeometry;
}

}  // namespace blue::weapon
