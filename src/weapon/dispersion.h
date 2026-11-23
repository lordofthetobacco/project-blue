#ifndef BLUE_WEAPON_DISPERSION_H_
#define BLUE_WEAPON_DISPERSION_H_

#include <cstdint>

#include <glm/vec3.hpp>

namespace blue::weapon {

// Radians per minute of angle (1/60 degree).
inline constexpr float kRadiansPerMoa = 2.908882e-4f;

// Per-shot dispersion of a weapon's bullets: each shot leaves in a direction
// spread uniformly over a cone whose angular diameter is the weapon's
// accuracy in MOA (a "4 MOA" pistol groups within 4 MOA), around the bore.
//
// Deterministic: a fixed seed gives the same sequence (xorshift32, like
// Viewmodel's recoil spread). A figure of 0 (or less, or not finite) leaves
// the direction exactly as it is and draws no numbers. No allocation.
// Copyable. Not thread-safe.
class Dispersion {
 public:
  explicit Dispersion(uint32_t seed = 0x5eed5u);

  // Returns unit `direction` turned by a random angle within the cone of
  // `accuracy_moa` diameter, uniform over the cone's disc.
  [[nodiscard]] glm::vec3 Apply(glm::vec3 direction, float accuracy_moa);

 private:
  // Returns the next value in [0, 1).
  float NextUnit();

  uint32_t state_ = 1;  // never 0
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_DISPERSION_H_
