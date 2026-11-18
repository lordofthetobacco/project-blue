#include "editor/viewport/editor_camera.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/vec4.hpp>

#include "camera/projection.h"

namespace blue::editor {
namespace {

// Pitch stays this far from straight up / down so the view never flips.
constexpr float kMaxPitch = std::numbers::pi_v<float> * 0.5f - 0.01f;
// Fraction of the distance one dolly step moves.
constexpr float kDollyStep = 0.15f;

}  // namespace

void EditorCamera::Orbit(glm::vec2 delta_radians) {
  yaw_ += delta_radians.x;
  pitch_ = std::clamp(pitch_ + delta_radians.y, -kMaxPitch, kMaxPitch);
}

void EditorCamera::SetOrientation(float yaw, float pitch) {
  yaw_ = yaw;
  pitch_ = std::clamp(pitch, -kMaxPitch, kMaxPitch);
}

void EditorCamera::Pan(glm::vec2 delta_pixels, float viewport_height) {
  const float height = std::max(viewport_height, 1.0f);
  // Metres per pixel at the target's depth.
  const float scale = 2.0f * distance_ * std::tan(fov_ * 0.5f) / height;
  const glm::mat4 world_from_view = glm::inverse(view());
  const glm::vec3 right(world_from_view[0]);
  const glm::vec3 up(world_from_view[1]);
  target_ += (-right * delta_pixels.x + up * delta_pixels.y) * scale;
}

void EditorCamera::Dolly(float steps) {
  distance_ = std::clamp(distance_ * std::pow(1.0f - kDollyStep, steps),
                         kMinDistance, kMaxDistance);
}

void EditorCamera::Look(glm::vec2 delta_radians) {
  const glm::vec3 eye = position();
  Orbit(delta_radians);
  target_ = eye + forward() * distance_;
}

void EditorCamera::Move(const glm::vec3& local) {
  const glm::mat4 world_from_view = glm::inverse(view());
  const glm::vec3 right(world_from_view[0]);
  const glm::vec3 up(world_from_view[1]);
  target_ += right * local.x + up * local.y + forward() * local.z;
}

void EditorCamera::Frame(const glm::vec3& center, float radius) {
  const float r = std::max(radius, 0.01f);
  target_ = center;
  // The sphere fits the vertical field of view, with a little margin.
  distance_ =
      std::clamp(1.2f * r / std::sin(fov_ * 0.5f), kMinDistance, kMaxDistance);
}

void EditorCamera::LookAt(const glm::vec3& eye, const glm::vec3& target) {
  const glm::vec3 offset = target - eye;
  const float length = glm::length(offset);
  if (!(length > 1.0e-6f)) {
    return;
  }
  const glm::vec3 direction = offset / length;
  target_ = target;
  distance_ = std::clamp(length, kMinDistance, kMaxDistance);
  pitch_ = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)),
                      -kMaxPitch, kMaxPitch);
  // forward() = (-sin(yaw) cos(pitch), sin(pitch), -cos(yaw) cos(pitch)).
  yaw_ = std::atan2(-direction.x, -direction.z);
}

glm::vec3 EditorCamera::forward() const {
  const float cos_pitch = std::cos(pitch_);
  return {-std::sin(yaw_) * cos_pitch, std::sin(pitch_),
          -std::cos(yaw_) * cos_pitch};
}

glm::vec3 EditorCamera::position() const {
  return target_ - forward() * distance_;
}

glm::mat4 EditorCamera::view() const {
  return glm::lookAt(position(), target_, glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 EditorCamera::projection(float aspect) const {
  return camera::ReversedInfinitePerspective(fov_, std::max(aspect, 1.0e-3f),
                                             near_);
}

Ray EditorCamera::ScreenRay(glm::vec2 ndc, float aspect) const {
  const float tan_half = std::tan(fov_ * 0.5f);
  const glm::vec3 view_direction(ndc.x * tan_half * aspect, ndc.y * tan_half,
                                 -1.0f);
  const glm::mat4 world_from_view = glm::inverse(view());
  const glm::vec3 direction = glm::normalize(
      glm::vec3(world_from_view * glm::vec4(view_direction, 0.0f)));
  return {.origin = position(), .direction = direction};
}

}  // namespace blue::editor
