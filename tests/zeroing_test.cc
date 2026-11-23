#include "weapon/zeroing.h"

#include <doctest/doctest.h>

#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "weapon/projectile_system.h"
#include "weapon/scope.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_geometry.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;

// The SPR A3G as calibrated by its prefab sockets (metres, scale 1): the
// sight on the scope's axis at the eyepiece's rear face, looking down -Z.
WeaponGeometry SniperGeometry() {
  return {.scale = 1.0f,
          .muzzle = {0.0f, 0.1376f, -0.5698f},
          .sight_eye_point = {0.0f, 0.1821f, 0.2392f},
          .sight_axis = {0.0f, 0.0f, -1.0f},
          .optic_lens = {0.0f, 0.1821f, 0.2392f},
          .optic_lens_radius = 0.0f,
          .grip = {0.0f, 0.06f, 0.275f},
          .eyepiece = {0.0f, 0.1821f, 0.2332f}};
}

// Returns a viewmodel of the rifle held fully aimed and still, level, eye at
// the origin.
Viewmodel AimedSniper(float zero) {
  Viewmodel viewmodel(SniperGeometry(), SprA3GViewmodelTuning());
  viewmodel.SetZeroDistance(zero);
  const ViewmodelInput input{.aim_held = true, .dt = kFrame};
  for (int i = 0; i < 300; ++i) {
    viewmodel.Update(input);
  }
  return viewmodel;
}

// Flies a bullet from the posed muzzle along the posed bore with the game's
// integrator (ProjectileSystem's 1/240 s substeps) and returns its height
// above the line of sight (the optic axis through the eye at the origin)
// where it is `distance` m down the line.
float HeightOverSightLine(const Viewmodel& viewmodel, float distance) {
  const FirearmSpec spec = SprA3GSpec();
  const glm::vec3 axis = viewmodel.optic_axis();
  const glm::vec3 up = viewmodel.lens_up();
  glm::vec3 position = viewmodel.muzzle_position();
  glm::vec3 velocity = viewmodel.muzzle_direction() * spec.muzzle_velocity;
  const glm::vec3 gravity(0.0f, -9.81f, 0.0f);
  for (int step = 0; step < 2400; ++step) {
    const BallisticStep next = IntegrateBallisticStep(
        velocity, gravity, spec.bullet_drag, 1.0f / 240.0f);
    const glm::vec3 end = position + next.displacement;
    const float before = glm::dot(position, axis);
    const float after = glm::dot(end, axis);
    if (after >= distance) {
      const float t = (distance - before) / (after - before);
      return glm::dot(position + t * (end - position), up);
    }
    position = end;
    velocity = next.velocity;
  }
  return -1.0e9f;
}

}  // namespace

TEST_CASE("Without drag the zero is the closed-form drag-free angle") {
  const ZeroSetup setup{.muzzle_below = 0.06f,
                        .muzzle_ahead = 0.8f,
                        .muzzle_velocity = 948.0f,
                        .drag = 0.0f};
  const float angle = ZeroAngle(setup, 50.0f);
  CHECK(angle > 0.0f);
  // The integrator is exact without drag: the bullet meets the line there.
  CHECK(std::abs(HeightAtDistance(setup, angle, 50.0f)) < 1.0e-4f);
  CHECK(ZeroAngle(setup, 0.0f) == 0.0f);
  CHECK(ZeroAngle({.muzzle_velocity = 0.0f}, 100.0f) == 0.0f);
}

TEST_CASE("With drag the zero needs more elevation and still meets the line") {
  ZeroSetup setup{.muzzle_below = 0.0445f,
                  .muzzle_ahead = 0.899f,
                  .muzzle_velocity = 790.0f,
                  .drag = 7.3e-4f};
  const float with_drag = ZeroAngle(setup, 300.0f);
  CHECK(std::abs(HeightAtDistance(setup, with_drag, 300.0f)) < 1.0e-3f);
  setup.drag = 0.0f;
  CHECK(with_drag > ZeroAngle(setup, 300.0f));
}

TEST_CASE("The SPR A3G's zero table, 100 to 300 m") {
  // Elevation of the bore over the scope's axis, mrad: the sight 4.45 cm
  // over the bore, the muzzle 0.90 m ahead of the eye, 790 m/s, drag
  // 7.3e-4 1/m.
  const ScopeSpec scope = SprA3GScope();
  constexpr float kTable[][2] = {{100.0f, 1.267f},
                                 {150.0f, 1.560f},
                                 {200.0f, 1.952f},
                                 {250.0f, 2.397f},
                                 {300.0f, 2.883f}};
  float zero = scope.start_zero;
  for (const auto& [distance, mrad] : kTable) {
    CAPTURE(distance);
    CHECK(zero == distance);
    const Viewmodel viewmodel = AimedSniper(zero);
    CHECK(viewmodel.zero_distance() == distance);
    CHECK(viewmodel.zero_angle() * 1000.0f ==
          doctest::Approx(mrad).epsilon(0.005));
    // A shot crosses the line of sight at the zero within a centimetre.
    CHECK(std::abs(HeightOverSightLine(viewmodel, distance)) < 0.01f);
    zero = StepZero(scope, zero, 1);
  }
  CHECK(zero == 300.0f);  // Clamped at the top.
}

TEST_CASE("Re-zeroing moves the point of impact by the trajectory") {
  // Zeroed at 100 m the bullet is low at 300 m by about half a metre, and
  // zeroed at 300 m it is high at 100 m.
  const float low = HeightOverSightLine(AimedSniper(100.0f), 300.0f);
  CHECK(low < -0.4f);
  CHECK(low > -0.6f);
  const float high = HeightOverSightLine(AimedSniper(300.0f), 100.0f);
  CHECK(high > 0.1f);
  CHECK(high < 0.2f);
}

}  // namespace blue::weapon
