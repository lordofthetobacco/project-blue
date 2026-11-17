#include "weapon/rifle_geometry.h"

namespace blue::weapon {

const RifleGeometry& M16Geometry() {
  static constexpr RifleGeometry kGeometry{
      .scale = 0.0771f,
      .muzzle = {0.0f, 0.598f, 8.995f},
      .optic_eye_side = {0.0f, 1.44f, -1.505f},
      .optic_axis = {0.0f, 0.0f, 1.0f},
      .optic_lens = {0.0f, 1.354f, -1.27f},
      .optic_lens_radius = 0.11f,
      .grip = {0.0f, -0.75f, -2.0f},
  };
  return kGeometry;
}

}  // namespace blue::weapon
