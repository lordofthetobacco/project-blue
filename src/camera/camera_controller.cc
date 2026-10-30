#include "camera/camera_controller.h"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include "camera/look.h"

namespace blue::camera {
namespace {

constexpr float kMinFlySpeed = 0.25f;
constexpr float kMaxFlySpeed = 100.0f;
constexpr float kFlySpeedPerNotch = 1.2f;
constexpr float kSprintMultiplier = 4.0f;
constexpr float kMinOrbitDistance = 0.25f;
constexpr float kMaxOrbitDistance = 500.0f;
constexpr float kOrbitDistancePerNotch = 0.9f;
// World units panned per device count per unit of orbit distance; scaling by
// distance keeps the scene under the cursor at any zoom level.
constexpr float kPanPerCount = 0.0015f;
constexpr float kMinDirectionLength = 1.0e-6f;

}  // namespace

void CameraController::SetFromSceneCamera(const scene::SceneCamera& camera) {
  const glm::vec3 offset = camera.target - camera.position;
  const float distance = glm::length(offset);

  position_ = camera.position;
  vertical_fov_degrees_ = camera.vertical_fov_degrees;

  const LookAngles look = LookTowards(camera.position, camera.target);
  yaw_ = look.yaw;
  pitch_ = look.pitch;
  orbit_distance_ =
      !std::isfinite(distance) || distance < kMinDirectionLength ? 1.0f
                                                                  : distance;
}

void CameraController::SetPose(glm::vec3 position, LookAngles look) {
  position_ = position;
  yaw_ = look.yaw;
  pitch_ = look.pitch;
}

scene::SceneCamera CameraController::ToSceneCamera() const {
  return {
      .position = position_,
      .target = Target(),
      .vertical_fov_degrees = vertical_fov_degrees_,
  };
}

void CameraController::Update(const input::InputFrame& input,
                              const input::InputSettings& settings,
                              float delta_seconds) {
  if (mode_ == CameraMode::kFly) {
    UpdateFly(input, settings, delta_seconds);
  } else {
    UpdateOrbit(input, settings);
  }
}

glm::vec3 CameraController::Forward() const {
  return ForwardFrom({.yaw = yaw_, .pitch = pitch_});
}

glm::mat4 CameraController::View() const {
  return glm::lookAt(position_, position_ + Forward(), kWorldUp);
}

glm::vec3 CameraController::Right() const {
  return glm::normalize(glm::cross(Forward(), kWorldUp));
}

glm::vec3 CameraController::Target() const {
  return position_ + Forward() * orbit_distance_;
}

void CameraController::ApplyLook(glm::vec2 mouse_delta,
                                 const input::InputSettings& settings) {
  const LookAngles look =
      ApplyMouseLook({.yaw = yaw_, .pitch = pitch_}, mouse_delta, settings);
  yaw_ = look.yaw;
  pitch_ = look.pitch;
}

void CameraController::UpdateFly(const input::InputFrame& input,
                                 const input::InputSettings& settings,
                                 float delta_seconds) {
  if (input.wheel != 0.0f) {
    fly_speed_ =
        std::clamp(fly_speed_ * std::pow(kFlySpeedPerNotch, input.wheel),
                   kMinFlySpeed, kMaxFlySpeed);
  }

  // `look` gates only the mouse: keys keep moving the camera while the cursor
  // is free.
  if (input.look) {
    ApplyLook(input.mouse_delta, settings);
  }

  glm::vec3 direction = Forward() * input.move.z + Right() * input.move.x +
                        kWorldUp * input.move.y;
  const float length = glm::length(direction);
  if (length > 1.0f) {
    direction /= length;
  }
  const float speed = fly_speed_ * (input.sprint ? kSprintMultiplier : 1.0f);
  position_ += direction * (speed * delta_seconds);
}

void CameraController::UpdateOrbit(const input::InputFrame& input,
                                   const input::InputSettings& settings) {
  glm::vec3 pivot = Target();

  if (input.pan) {
    const glm::vec3 cam_right = Right();
    const glm::vec3 cam_up = glm::cross(cam_right, Forward());
    const float scale = kPanPerCount * orbit_distance_;
    // Dragging moves the scene with the cursor, so the pivot (and camera) go
    // the opposite way.
    pivot += (-cam_right * input.mouse_delta.x + cam_up * input.mouse_delta.y) *
             scale;
  } else if (input.look) {
    ApplyLook(input.mouse_delta, settings);
  }

  if (input.wheel != 0.0f) {
    orbit_distance_ = std::clamp(
        orbit_distance_ * std::pow(kOrbitDistancePerNotch, input.wheel),
        kMinOrbitDistance, kMaxOrbitDistance);
  }

  position_ = pivot - Forward() * orbit_distance_;
}

}  // namespace blue::camera
