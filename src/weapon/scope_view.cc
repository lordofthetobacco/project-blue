#include "weapon/scope_view.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/geometric.hpp>
#include <glm/vec4.hpp>

namespace blue::weapon {
namespace {

constexpr float kMinGlassPixels = 8.0f;
constexpr uint32_t kPictureSizeStep = 32;

// Returns the world -> view matrix of a camera at `position` with unit
// `right`, `up` and forward `axis` (view space looks down -z).
glm::mat4 ViewMatrix(glm::vec3 position, glm::vec3 right, glm::vec3 up,
                     glm::vec3 axis) {
  // The inverse of the camera-to-world frame (right, up, -axis, position):
  // the rotation is orthonormal, so its inverse is the transpose.
  glm::mat4 view(1.0f);
  view[0] = glm::vec4(right.x, up.x, -axis.x, 0.0f);
  view[1] = glm::vec4(right.y, up.y, -axis.y, 0.0f);
  view[2] = glm::vec4(right.z, up.z, -axis.z, 0.0f);
  view[3] = glm::vec4(-glm::dot(right, position), -glm::dot(up, position),
                      glm::dot(axis, position), 1.0f);
  return view;
}

}  // namespace

ScopeViewState MakeScopeViewState(const ScopeSpec& spec,
                                  const Viewmodel& viewmodel, glm::vec3 eye,
                                  float magnification,
                                  const ScopeScreen& screen) {
  const ScopeOptics& optics = spec.optics;
  const float m = std::max(magnification, 1.0f);
  const glm::vec3 right = viewmodel.lens_right();
  const glm::vec3 up = viewmodel.lens_up();
  const glm::vec3 axis = viewmodel.optic_axis();
  const glm::vec3 eyepiece = viewmodel.eyepiece_center();

  ScopeViewState state;
  state.camera_position = eyepiece + axis * optics.objective_distance;
  state.camera_view = ViewMatrix(state.camera_position, right, up, axis);
  state.camera_tangent = ScopeCameraTangent(optics, m);

  const glm::vec3 glass = eyepiece - axis * kEyepieceLift;
  state.lens = glm::mat4(1.0f);
  state.lens[0] = glm::vec4(right * optics.eyepiece_radius, 0.0f);
  state.lens[1] = glm::vec4(up * optics.eyepiece_radius, 0.0f);
  state.lens[2] = glm::vec4(axis, 0.0f);
  state.lens[3] = glm::vec4(glass, 1.0f);

  state.eye = PlaceEye(optics, eye, eyepiece, right, up, axis);
  state.exit_pupil_radius = ExitPupilRadius(optics, m);
  state.peak_transmission = PeakEyeBoxTransmission(optics, m, state.eye);

  // The glass on screen: its diameter over its distance, in pixels of the
  // main view.
  const glm::vec3 to_glass = glass - eye;
  const float distance = glm::length(to_glass);
  const bool behind_glass = glm::dot(to_glass, axis) > 0.0f;
  const float tan_half_fov =
      std::tan(std::max(screen.vertical_fov_radians, 1.0e-3f) * 0.5f);
  if (behind_glass && distance > 0.0f) {
    state.glass_pixels = 2.0f * optics.eyepiece_radius / distance *
                         (0.5f * screen.height_pixels) / tan_half_fov;
  }
  // The picture spans camera_tangent * M of apparent tangent where the glass
  // spans apparent_field: scale the glass's pixels by that.
  const float field = std::max(optics.apparent_field, 1.0e-6f);
  const float wanted = state.glass_pixels * state.camera_tangent * m / field;
  const auto steps = static_cast<uint32_t>(
      std::ceil(std::min(wanted, static_cast<float>(kMaxScopePictureSize)) /
                static_cast<float>(kPictureSizeStep)));
  state.picture_size = std::clamp(steps * kPictureSizeStep,
                                  kMinScopePictureSize, kMaxScopePictureSize);
  state.render_picture = behind_glass &&
                         state.glass_pixels >= kMinGlassPixels &&
                         state.peak_transmission >= kMinScopeTransmission;
  return state;
}

float ScopeLookScale(float magnification, float aim_fraction) {
  const float aim = std::clamp(aim_fraction, 0.0f, 1.0f);
  return 1.0f + (1.0f / std::max(magnification, 1.0f) - 1.0f) * aim;
}

}  // namespace blue::weapon
