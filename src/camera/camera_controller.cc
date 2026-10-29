#include "camera/camera_controller.h"

#include <algorithm>
#include <cmath>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

namespace blue::camera {
namespace {

constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};
constexpr float kMaxPitch = glm::radians(89.0f);
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

// Returns `yaw` wrapped into (-pi, pi].
float WrapYaw(float yaw) {
  // remainder() maps to [-pi, pi]; the interval must be half-open at -pi.
  const float wrapped = std::remainder(yaw, glm::two_pi<float>());
  return wrapped <= -glm::pi<float>() ? glm::pi<float>() : wrapped;
}

}  // namespace

void CameraController::SetFromSceneCamera(const scene::SceneCamera& camera) {
  const glm::vec3 offset = camera.target - camera.position;
  const float distance = glm::length(offset);

  position_ = camera.position;
  vertical_fov_degrees_ = camera.vertical_fov_degrees;

  if (!std::isfinite(distance) || distance < kMinDirectionLength) {
    yaw_ = 0.0f;
    pitch_ = 0.0f;
    orbit_distance_ = 1.0f;
    return;
  }

  const glm::vec3 direction = offset / distance;
  yaw_ = std::atan2(-direction.x, -direction.z);
  pitch_ = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)),
                      -kMaxPitch, kMaxPitch);
  orbit_distance_ = distance;
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
  const float cos_pitch = std::cos(pitch_);
  return {-std::sin(yaw_) * cos_pitch, std::sin(pitch_),
          -std::cos(yaw_) * cos_pitch};
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
  // Deltas are raw device counts: no dt scaling, so the same hand movement
  // turns the view by the same angle at any frame rate.
  const float vertical_sign = settings.invert_y ? 1.0f : -1.0f;
  yaw_ = WrapYaw(yaw_ - mouse_delta.x * settings.mouse_sensitivity);
  pitch_ = std::clamp(
      pitch_ + vertical_sign * mouse_delta.y * settings.mouse_sensitivity,
      -kMaxPitch, kMaxPitch);
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
