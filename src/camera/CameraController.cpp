#include "camera/CameraController.hpp"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>

namespace blue {

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
// World units panned per device count per unit of orbit distance; scaling by distance keeps the
// scene under the cursor at any zoom level.
constexpr float kPanPerCount = 0.0015f;
constexpr float kMinDirectionLength = 1e-6f;

float wrapYaw(float yaw)
{
    // remainder() maps to [-pi, pi]; the interval must be half-open at -pi.
    const float wrapped = std::remainder(yaw, glm::two_pi<float>());
    return wrapped <= -glm::pi<float>() ? glm::pi<float>() : wrapped;
}

} // namespace

void CameraController::setFromSceneCamera(const SceneCamera& camera)
{
    const glm::vec3 offset = camera.target - camera.position;
    const float distance = glm::length(offset);

    position_ = camera.position;
    verticalFovDegrees_ = camera.verticalFovDegrees;

    if (!std::isfinite(distance) || distance < kMinDirectionLength) {
        yaw_ = 0.0f;
        pitch_ = 0.0f;
        orbitDistance_ = 1.0f;
        return;
    }

    const glm::vec3 direction = offset / distance;
    yaw_ = std::atan2(-direction.x, -direction.z);
    pitch_ = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)), -kMaxPitch, kMaxPitch);
    orbitDistance_ = distance;
}

SceneCamera CameraController::toSceneCamera() const
{
    return {
        .position = position_,
        .target = target(),
        .verticalFovDegrees = verticalFovDegrees_,
    };
}

void CameraController::setMode(CameraMode mode)
{
    mode_ = mode;
}

CameraMode CameraController::mode() const
{
    return mode_;
}

void CameraController::update(const InputFrame& input, const InputSettings& settings, float deltaSeconds)
{
    if (mode_ == CameraMode::Fly) {
        updateFly(input, settings, deltaSeconds);
    } else {
        updateOrbit(input, settings);
    }
}

glm::vec3 CameraController::position() const
{
    return position_;
}

glm::vec3 CameraController::forward() const
{
    const float cosPitch = std::cos(pitch_);
    return {-std::sin(yaw_) * cosPitch, std::sin(pitch_), -std::cos(yaw_) * cosPitch};
}

float CameraController::yaw() const
{
    return yaw_;
}

float CameraController::pitch() const
{
    return pitch_;
}

float CameraController::flySpeed() const
{
    return flySpeed_;
}

glm::mat4 CameraController::view() const
{
    return glm::lookAt(position_, position_ + forward(), kWorldUp);
}

float CameraController::verticalFovDegrees() const
{
    return verticalFovDegrees_;
}

glm::vec3 CameraController::right() const
{
    return glm::normalize(glm::cross(forward(), kWorldUp));
}

glm::vec3 CameraController::target() const
{
    return position_ + forward() * orbitDistance_;
}

void CameraController::applyLook(glm::vec2 mouseDelta, const InputSettings& settings)
{
    // Deltas are raw device counts: no dt scaling, so the same hand movement turns the view by the
    // same angle at any frame rate.
    const float verticalSign = settings.invertY ? 1.0f : -1.0f;
    yaw_ = wrapYaw(yaw_ - mouseDelta.x * settings.mouseSensitivity);
    pitch_ = std::clamp(pitch_ + verticalSign * mouseDelta.y * settings.mouseSensitivity, -kMaxPitch, kMaxPitch);
}

void CameraController::updateFly(const InputFrame& input, const InputSettings& settings, float deltaSeconds)
{
    if (input.wheel != 0.0f) {
        flySpeed_ = std::clamp(flySpeed_ * std::pow(kFlySpeedPerNotch, input.wheel), kMinFlySpeed, kMaxFlySpeed);
    }

    // `look` gates only the mouse: keys keep moving the camera while the cursor is free.
    if (input.look) {
        applyLook(input.mouseDelta, settings);
    }

    glm::vec3 direction = forward() * input.move.z + right() * input.move.x + kWorldUp * input.move.y;
    const float length = glm::length(direction);
    if (length > 1.0f) {
        direction /= length;
    }
    const float speed = flySpeed_ * (input.sprint ? kSprintMultiplier : 1.0f);
    position_ += direction * (speed * deltaSeconds);
}

void CameraController::updateOrbit(const InputFrame& input, const InputSettings& settings)
{
    glm::vec3 pivot = target();

    if (input.pan) {
        const glm::vec3 camRight = right();
        const glm::vec3 camUp = glm::cross(camRight, forward());
        const float scale = kPanPerCount * orbitDistance_;
        // Dragging moves the scene with the cursor, so the pivot (and camera) go the opposite way.
        pivot += (-camRight * input.mouseDelta.x + camUp * input.mouseDelta.y) * scale;
    } else if (input.look) {
        applyLook(input.mouseDelta, settings);
    }

    if (input.wheel != 0.0f) {
        orbitDistance_ = std::clamp(orbitDistance_ * std::pow(kOrbitDistancePerNotch, input.wheel),
                                    kMinOrbitDistance, kMaxOrbitDistance);
    }

    position_ = pivot - forward() * orbitDistance_;
}

} // namespace blue
