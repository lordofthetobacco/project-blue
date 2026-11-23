#include "weapon/scope_optics.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <glm/geometric.hpp>

namespace blue::weapon {
namespace {

// ReticleDistance() without lines.
constexpr float kFarFromReticle = 1.0e3f;
// Headroom of the scope camera's field over what the lens can show.
constexpr float kCameraFieldMargin = 1.02f;

// Returns 1 at or below `edge0`, 0 at or above `edge1` (edge0 < edge1) and a
// smooth ramp between.
float SmoothFall(float edge0, float edge1, float x) {
  const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return 1.0f - t * t * (3.0f - 2.0f * t);
}

// Returns the distance from `point` to the segment `from`-`to`.
float SegmentDistance(glm::vec2 point, glm::vec2 from, glm::vec2 to) {
  const glm::vec2 along = to - from;
  const float length_sq = glm::dot(along, along);
  const float t =
      length_sq > 0.0f
          ? std::clamp(glm::dot(point - from, along) / length_sq, 0.0f, 1.0f)
          : 0.0f;
  return glm::length(point - (from + t * along));
}

}  // namespace

Reticle SimpleCrossReticle(float half_width) {
  Reticle reticle;
  reticle.lines[0] = {
      .from = {-1.0f, 0.0f}, .to = {1.0f, 0.0f}, .half_width = half_width};
  reticle.lines[1] = {
      .from = {0.0f, -1.0f}, .to = {0.0f, 1.0f}, .half_width = half_width};
  reticle.line_count = 2;
  return reticle;
}

float ReticleDistance(const Reticle& reticle, const ScopeOptics& optics,
                      glm::vec2 point) {
  float nearest = kFarFromReticle;
  const int count = std::clamp(reticle.line_count, 0, kMaxReticleLines);
  for (int i = 0; i < count; ++i) {
    const ReticleLine& line = reticle.lines[static_cast<size_t>(i)];
    nearest = std::min(nearest,
                       SegmentDistance(point, line.from * optics.apparent_field,
                                       line.to * optics.apparent_field) -
                           line.half_width);
  }
  return nearest;
}

float ExitPupilRadius(const ScopeOptics& optics, float magnification) {
  return optics.objective_radius / std::max(magnification, 1.0f);
}

EyePlacement PlaceEye(const ScopeOptics& optics, glm::vec3 eye,
                      glm::vec3 eyepiece, glm::vec3 right, glm::vec3 up,
                      glm::vec3 axis) {
  const glm::vec3 offset = eye - eyepiece;
  return {.lateral = {glm::dot(offset, right), glm::dot(offset, up)},
          .relief_error = -glm::dot(offset, axis) - optics.eye_relief};
}

float EyeBoxTransmission(const ScopeOptics& optics, float magnification,
                         const EyePlacement& placement, glm::vec2 apparent) {
  const glm::vec2 crossing =
      placement.lateral +
      apparent * (placement.relief_error + optics.pupil_shadow_depth);
  const float exit_pupil = ExitPupilRadius(optics, magnification);
  const float eye_pupil = std::max(optics.eye_pupil_radius, 1.0e-5f);
  // Full while the eye pupil's centre is inside the exit pupil, none once
  // the two discs are apart.
  return SmoothFall(exit_pupil, exit_pupil + eye_pupil, glm::length(crossing));
}

float PeakEyeBoxTransmission(const ScopeOptics& optics, float magnification,
                             const EyePlacement& placement) {
  const float spread = placement.relief_error + optics.pupil_shadow_depth;
  glm::vec2 best(0.0f);
  if (std::abs(spread) > 1.0e-6f) {
    best = -placement.lateral / spread;
    const float length = glm::length(best);
    if (length > optics.apparent_field) {
      best *= optics.apparent_field / length;
    }
  }
  return EyeBoxTransmission(optics, magnification, placement, best);
}

glm::vec2 DistortApparent(const ScopeOptics& optics, glm::vec2 apparent,
                          float channel_aberration) {
  const float field = std::max(optics.apparent_field, 1.0e-6f);
  const float r = glm::length(apparent) / field;
  return apparent * (1.0f + (optics.distortion + channel_aberration) * r * r);
}

float Vignetting(const ScopeOptics& optics, glm::vec2 apparent) {
  const float field = std::max(optics.apparent_field, 1.0e-6f);
  const float r = std::min(glm::length(apparent) / field, 1.0f);
  const float r2 = r * r;
  return 1.0f - std::clamp(optics.vignetting, 0.0f, 1.0f) * r2 * r2;
}

float ScopeCameraTangent(const ScopeOptics& optics, float magnification) {
  const float reach = 1.0f + std::max(optics.distortion, 0.0f) +
                      std::abs(optics.chromatic_aberration);
  return optics.apparent_field * reach * kCameraFieldMargin /
         std::max(magnification, 1.0f);
}

}  // namespace blue::weapon
