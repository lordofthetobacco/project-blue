#include "weapon/scope.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "weapon/scope_optics.h"
#include "weapon/scope_view.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_geometry.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon {
namespace {

constexpr float kFrame = 1.0f / 60.0f;

ScopeSpec TestScope() {
  return {.min_magnification = 3.5f,
          .max_magnification = 10.0f,
          .step = 0.5f,
          .start_magnification = 3.5f,
          .zoom_time = 0.12f};
}

// Returns the fraction of the way from `from` to `to` that `value` has gone,
// measured in log space (how the zoom blends).
float LogProgress(float from, float to, float value) {
  return std::log(value / from) / std::log(to / from);
}

}  // namespace

TEST_CASE("The SPR A3G's scope is a 3.5-10x in half steps from 3.5x") {
  const ScopeSpec scope = SprA3GScope();
  CHECK(scope.min_magnification == 3.5f);
  CHECK(scope.max_magnification == 10.0f);
  CHECK(scope.step == 0.5f);
  CHECK(scope.start_magnification == 3.5f);
}

TEST_CASE("A scope zoom starts settled and clamped to its range") {
  const ScopeZoom zoom(TestScope(), 6.0f);
  CHECK(zoom.target() == 6.0f);
  CHECK(zoom.current() == 6.0f);
  CHECK(ScopeZoom(TestScope(), 0.0f).target() == 3.5f);
  CHECK(ScopeZoom(TestScope(), 40.0f).target() == 10.0f);
}

TEST_CASE("Each wheel notch steps the magnification, clamped to the range") {
  ScopeZoom zoom(TestScope(), 3.5f);
  CHECK(zoom.AddWheel(1.0f));  // Wheel up zooms in.
  CHECK(zoom.target() == doctest::Approx(4.0f));
  CHECK(zoom.AddWheel(3.0f));
  CHECK(zoom.target() == doctest::Approx(5.5f));
  CHECK(zoom.AddWheel(-2.0f));
  CHECK(zoom.target() == doctest::Approx(4.5f));

  CHECK(zoom.AddWheel(50.0f));
  CHECK(zoom.target() == 10.0f);
  CHECK_FALSE(zoom.AddWheel(1.0f));  // Already at the top.
  CHECK(zoom.target() == 10.0f);
  CHECK(zoom.AddWheel(-50.0f));
  CHECK(zoom.target() == 3.5f);
  CHECK_FALSE(zoom.AddWheel(-1.0f));
  CHECK_FALSE(zoom.AddWheel(0.0f));
  CHECK_FALSE(zoom.AddWheel(std::numeric_limits<float>::quiet_NaN()));
  CHECK(zoom.target() == 3.5f);
}

TEST_CASE("Fractional wheel notches add up to whole steps") {
  ScopeZoom zoom(TestScope(), 5.0f);
  CHECK_FALSE(zoom.AddWheel(0.4f));
  CHECK_FALSE(zoom.AddWheel(0.4f));
  CHECK(zoom.target() == 5.0f);
  CHECK(zoom.AddWheel(0.4f));  // 1.2 notches: one step, 0.2 kept.
  CHECK(zoom.target() == doctest::Approx(5.5f));
  CHECK(zoom.AddWheel(0.8f));  // 0.2 + 0.8: another whole notch.
  CHECK(zoom.target() == doctest::Approx(6.0f));
  CHECK_FALSE(zoom.AddWheel(-0.6f));
  CHECK(zoom.AddWheel(-0.6f));  // -1.2: one step out.
  CHECK(zoom.target() == doctest::Approx(5.5f));
}

TEST_CASE("The shown magnification follows the chosen one smoothly") {
  ScopeZoom zoom(TestScope(), 4.0f);
  REQUIRE(zoom.AddWheel(12.0f));
  REQUIRE(zoom.target() == 10.0f);
  CHECK(zoom.current() == 4.0f);  // Nothing moves before an update.

  zoom.Update(kFrame);
  const float first = zoom.current();
  CHECK(first > 4.0f);
  CHECK(first < 6.0f);  // Not a jump.

  // About 95 % (in log space) zoom_time after the start, never past the
  // target.
  constexpr int kSteps = 8;
  const float step = (TestScope().zoom_time - kFrame) / kSteps;
  for (int i = 0; i < kSteps; ++i) {
    zoom.Update(step);
    CHECK(zoom.current() <= 10.0f);
  }
  CHECK(LogProgress(4.0f, 10.0f, zoom.current()) ==
        doctest::Approx(0.95f).epsilon(0.01));

  for (int i = 0; i < 120; ++i) {
    zoom.Update(kFrame);
  }
  CHECK(zoom.current() == 10.0f);  // Settles exactly.
  zoom.Update(-1.0f);              // A negative step does nothing.
  CHECK(zoom.current() == 10.0f);
}

TEST_CASE("The zoom blend is frame-rate independent") {
  ScopeZoom coarse(TestScope(), 3.5f);
  ScopeZoom fine(TestScope(), 3.5f);
  REQUIRE(coarse.AddWheel(8.0f));
  REQUIRE(fine.AddWheel(8.0f));
  coarse.Update(0.06f);
  for (int i = 0; i < 6; ++i) {
    fine.Update(0.01f);
  }
  CHECK(coarse.current() == doctest::Approx(fine.current()).epsilon(1.0e-4));
}

TEST_CASE("The zero turret clicks in 50 m steps from 100 to 300 m") {
  const ScopeSpec scope = SprA3GScope();
  CHECK(scope.start_zero == 100.0f);
  CHECK(ClampZero(scope, 20.0f) == 100.0f);
  CHECK(ClampZero(scope, 1000.0f) == 300.0f);
  CHECK(StepZero(scope, 100.0f, 1) == 150.0f);
  CHECK(StepZero(scope, 150.0f, 2) == 250.0f);
  CHECK(StepZero(scope, 250.0f, 3) == 300.0f);  // Clamped.
  CHECK(StepZero(scope, 200.0f, -1) == 150.0f);
  CHECK(StepZero(scope, 100.0f, -1) == 100.0f);
  CHECK(StepZero(scope, 0.0f, 0) == 100.0f);  // Brought into range.
}

TEST_CASE("The exit pupil shrinks with the magnification") {
  const ScopeOptics optics = SprA3GScope().optics;
  CHECK(ExitPupilRadius(optics, 3.5f) == doctest::Approx(0.020f / 3.5f));
  CHECK(ExitPupilRadius(optics, 10.0f) == doctest::Approx(0.002f));
  CHECK(ExitPupilRadius(optics, 0.5f) == doctest::Approx(0.020f));
}

TEST_CASE("The eye is placed against the eyepiece's frame") {
  const ScopeOptics optics = SprA3GScope().optics;
  // Scope looking down -Z with its eyepiece at the origin.
  const EyePlacement on_axis =
      PlaceEye(optics, {0.0f, 0.0f, optics.eye_relief}, glm::vec3(0.0f),
               {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  CHECK(glm::length(on_axis.lateral) < 1.0e-6f);
  CHECK(std::abs(on_axis.relief_error) < 1.0e-6f);
  const EyePlacement off = PlaceEye(
      optics, {0.003f, -0.002f, optics.eye_relief + 0.02f}, glm::vec3(0.0f),
      {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f});
  CHECK(off.lateral.x == doctest::Approx(0.003f));
  CHECK(off.lateral.y == doctest::Approx(-0.002f));
  CHECK(off.relief_error == doctest::Approx(0.02f));  // Too far back.
}

TEST_CASE("On the axis at the right relief the whole field is lit") {
  const ScopeOptics optics = SprA3GScope().optics;
  for (const float m : {3.5f, 6.0f, 10.0f}) {
    CAPTURE(m);
    CHECK(EyeBoxTransmission(optics, m, {}, glm::vec2(0.0f)) == 1.0f);
    // At most a trace of shading at the very edge at 10x.
    for (const glm::vec2 t :
         {glm::vec2(0.18f, 0.0f), glm::vec2(-0.12f, 0.12f)}) {
      CHECK(EyeBoxTransmission(optics, m, {}, t) >= 0.95f);
    }
    CHECK(PeakEyeBoxTransmission(optics, m, {}) == 1.0f);
  }
  CHECK(EyeBoxTransmission(optics, 3.5f, {}, {0.18f, 0.0f}) == 1.0f);
}

TEST_CASE("An eye off to one side sees a crescent of shadow from that side") {
  const ScopeOptics optics = SprA3GScope().optics;
  const EyePlacement right{.lateral = {0.004f, 0.0f}};  // 4 mm right
  const float toward = EyeBoxTransmission(optics, 10.0f, right, {0.15f, 0.0f});
  const float centre = EyeBoxTransmission(optics, 10.0f, right, {0.0f, 0.0f});
  const float away = EyeBoxTransmission(optics, 10.0f, right, {-0.15f, 0.0f});
  CHECK(toward < centre);
  CHECK(centre < away);
  CHECK(toward == 0.0f);  // The right edge is dark...
  CHECK(away > 0.5f);     // ...the left still lit.
  // At 3.5x the exit pupil is wide: the same offset loses much less.
  CHECK(EyeBoxTransmission(optics, 3.5f, right, {0.0f, 0.0f}) > 0.9f);
  CHECK(EyeBoxTransmission(optics, 3.5f, right, {0.15f, 0.0f}) > toward);
}

TEST_CASE("Too far back the field closes in to a tunnel, far off to black") {
  const ScopeOptics optics = SprA3GScope().optics;
  const EyePlacement back{.relief_error = 0.05f};  // 5 cm too far back
  CHECK(EyeBoxTransmission(optics, 10.0f, back, {0.0f, 0.0f}) == 1.0f);
  CHECK(EyeBoxTransmission(optics, 10.0f, back, {0.0f, 0.17f}) == 0.0f);
  const EyePlacement gone{.lateral = {0.0f, 0.03f}};  // 3 cm low
  CHECK(PeakEyeBoxTransmission(optics, 10.0f, gone) == 0.0f);
  // The peak finds the lit part of the field when there is one.
  const EyePlacement off{.lateral = {0.004f, 0.0f}, .relief_error = 0.02f};
  CHECK(PeakEyeBoxTransmission(optics, 10.0f, off) == 1.0f);
}

TEST_CASE("The eye box is smooth in the eye's position") {
  const ScopeOptics optics = SprA3GScope().optics;
  float previous = 1.0f;
  for (int i = 0; i <= 200; ++i) {
    const EyePlacement eye{.lateral = {static_cast<float>(i) * 0.0001f, 0.0f}};
    const float now = EyeBoxTransmission(optics, 10.0f, eye, {0.1f, 0.0f});
    CHECK(now <= previous);
    CHECK(previous - now < 0.05f);
    previous = now;
  }
  CHECK(previous == 0.0f);
}

TEST_CASE("Distortion, colour and vignetting grow towards the field's edge") {
  const ScopeOptics optics = SprA3GScope().optics;
  const glm::vec2 centre(0.0f);
  CHECK(DistortApparent(optics, centre, 0.0f) == centre);
  const glm::vec2 edge(optics.apparent_field, 0.0f);
  // Pincushion: the edge shows a little further out.
  CHECK(DistortApparent(optics, edge, 0.0f).x ==
        doctest::Approx(optics.apparent_field * (1.0f + optics.distortion)));
  const glm::vec2 half(0.5f * optics.apparent_field, 0.0f);
  CHECK(DistortApparent(optics, half, 0.0f).x ==
        doctest::Approx(half.x * (1.0f + 0.25f * optics.distortion)));
  // Red and blue split apart only towards the edge.
  const float red =
      DistortApparent(optics, edge, optics.chromatic_aberration).x;
  const float blue =
      DistortApparent(optics, edge, -optics.chromatic_aberration).x;
  CHECK(red > blue);
  CHECK(red - blue ==
        doctest::Approx(2.0f * optics.chromatic_aberration * edge.x));
  CHECK(Vignetting(optics, centre) == 1.0f);
  CHECK(Vignetting(optics, edge) == doctest::Approx(1.0f - optics.vignetting));
  CHECK(Vignetting(optics, 2.0f * edge) ==
        doctest::Approx(1.0f - optics.vignetting));
}

TEST_CASE("The scope camera covers every direction the lens shows") {
  const ScopeOptics optics = SprA3GScope().optics;
  for (const float m : {3.5f, 10.0f}) {
    CAPTURE(m);
    const float tangent = ScopeCameraTangent(optics, m);
    // The farthest source tangent: the field's corner through the red
    // channel's distortion, then the magnification.
    const float farthest =
        DistortApparent(optics, {optics.apparent_field, 0.0f},
                        optics.chromatic_aberration)
            .x /
        m;
    CHECK(tangent >= farthest);
    CHECK(tangent < farthest * 1.05f);  // And not much more: sharpness.
  }
}

TEST_CASE("A simple cross reticle: two thin lines through the centre") {
  const ScopeOptics optics = SprA3GScope().optics;
  const Reticle cross = SimpleCrossReticle(0.0004f);
  CHECK(cross.line_count == 2);
  CHECK(ReticleDistance(cross, optics, {0.0f, 0.0f}) ==
        doctest::Approx(-0.0004f));
  CHECK(ReticleDistance(cross, optics, {0.1f, 0.0f}) ==
        doctest::Approx(-0.0004f));  // On the horizontal line.
  CHECK(ReticleDistance(cross, optics, {0.05f, 0.03f}) ==
        doctest::Approx(0.03f - 0.0004f));
  // The lines end at the field stop.
  CHECK(ReticleDistance(cross, optics, {optics.apparent_field + 0.01f, 0.0f}) ==
        doctest::Approx(0.01f - 0.0004f));
  CHECK(ReticleDistance(Reticle{}, optics, {0.0f, 0.0f}) > 1.0f);  // No lines.
}

TEST_CASE("The look slows with the magnification as the scope comes up") {
  CHECK(ScopeLookScale(10.0f, 0.0f) == 1.0f);
  CHECK(ScopeLookScale(10.0f, 1.0f) == doctest::Approx(0.1f));
  CHECK(ScopeLookScale(4.0f, 0.5f) == doctest::Approx(0.625f));
  CHECK(ScopeLookScale(0.5f, 1.0f) == 1.0f);  // No magnification.
  CHECK(ScopeLookScale(4.0f, 3.0f) == doctest::Approx(0.25f));  // Clamped.
}

TEST_CASE("Fully aimed, the scope picture looks down the view axis") {
  const ScopeSpec scope = SprA3GScope();
  WeaponGeometry geometry{.scale = 1.0f,
                          .muzzle = {0.0f, 0.1376f, -0.5698f},
                          .sight_eye_point = {0.0f, 0.1821f, 0.2392f},
                          .sight_axis = {0.0f, 0.0f, -1.0f},
                          .optic_lens = {0.0f, 0.1821f, 0.2392f},
                          .optic_lens_radius = 0.0f,
                          .grip = {0.0f, 0.06f, 0.275f},
                          .eyepiece = {0.0f, 0.1821f, 0.2332f}};
  Viewmodel viewmodel(geometry, SprA3GViewmodelTuning());
  const ViewmodelInput aim{.aim_held = true, .dt = kFrame};
  for (int i = 0; i < 300; ++i) {
    viewmodel.Update(aim);
  }
  const ScopeScreen screen{.vertical_fov_radians = 0.98f,
                           .height_pixels = 1080.0f};
  const ScopeViewState state =
      MakeScopeViewState(scope, viewmodel, glm::vec3(0.0f), 10.0f, screen);
  // The eye sits on the axis at the eye relief: everything lit.
  CHECK(glm::length(state.eye.lateral) < 1.0e-4f);
  CHECK(std::abs(state.eye.relief_error) < 1.0e-3f);
  CHECK(state.peak_transmission == 1.0f);
  CHECK(state.render_picture);
  // The scope's camera: at the objective, straight ahead, looking along
  // the view (world -Z) with the world upright.
  CHECK(glm::length(state.camera_position -
                    glm::vec3(0.0f, 0.0f, -0.096f - 0.326f)) < 1.0e-3f);
  const glm::vec4 ahead =
      state.camera_view * glm::vec4(0.0f, 0.0f, -100.0f, 1.0f);
  CHECK(std::abs(ahead.x) < 1.0e-3f);
  CHECK(std::abs(ahead.y) < 1.0e-3f);
  CHECK(ahead.z < -99.0f);
  const glm::vec4 above =
      state.camera_view * glm::vec4(0.0f, 1.0f, -100.0f, 1.0f);
  CHECK(above.y == doctest::Approx(1.0f).epsilon(1.0e-3));
  CHECK(state.camera_tangent ==
        doctest::Approx(ScopeCameraTangent(scope.optics, 10.0f)));
  // The glass: 32.6 mm across 9.6 cm away is about 300 px at 1080p with
  // this view, the picture a little wider.
  CHECK(state.glass_pixels > 250.0f);
  CHECK(state.glass_pixels < 400.0f);
  CHECK(state.picture_size >= static_cast<uint32_t>(state.glass_pixels));
  CHECK(state.picture_size % 32 == 0);
  CHECK(state.picture_size <= kMaxScopePictureSize);
  // The glass disc on the eyepiece, lifted towards the eye.
  const glm::vec4 centre = state.lens * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  CHECK(centre.z == doctest::Approx(-0.096f + kEyepieceLift).epsilon(1.0e-2));
  const glm::vec4 rim = state.lens * glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
  CHECK(rim.x - centre.x == doctest::Approx(scope.optics.eyepiece_radius));
}

TEST_CASE("At the hip no light reaches the eye and nothing is rendered") {
  const ScopeSpec scope = SprA3GScope();
  WeaponGeometry geometry{.scale = 1.0f,
                          .muzzle = {0.0f, 0.1376f, -0.5698f},
                          .sight_eye_point = {0.0f, 0.1821f, 0.2392f},
                          .sight_axis = {0.0f, 0.0f, -1.0f},
                          .grip = {0.0f, 0.06f, 0.275f},
                          .eyepiece = {0.0f, 0.1821f, 0.2332f}};
  Viewmodel viewmodel(geometry, SprA3GViewmodelTuning());
  viewmodel.Update({.dt = kFrame});
  const ScopeViewState state = MakeScopeViewState(
      scope, viewmodel, glm::vec3(0.0f), 3.5f, ScopeScreen{});
  CHECK(state.peak_transmission == 0.0f);
  CHECK_FALSE(state.render_picture);
  CHECK(state.picture_size >= kMinScopePictureSize);
}

}  // namespace blue::weapon
