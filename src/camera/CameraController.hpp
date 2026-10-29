#pragma once

#include "input/InputFrame.hpp"
#include "input/InputSettings.hpp"
#include "scene/Scene.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

namespace blue {

enum class CameraMode { Fly, Orbit };

// Pure-math camera: free-fly and orbit share one state (position, yaw, pitch, orbit distance), so
// switching modes never moves the view. The orbit target is always position + forward * distance.
class CameraController {
public:
    void setFromSceneCamera(const SceneCamera& camera);
    [[nodiscard]] SceneCamera toSceneCamera() const;

    void setMode(CameraMode mode);
    [[nodiscard]] CameraMode mode() const;

    void update(const InputFrame& input, const InputSettings& settings, float deltaSeconds);

    [[nodiscard]] glm::vec3 position() const;
    [[nodiscard]] glm::vec3 forward() const;
    [[nodiscard]] float yaw() const;   // radians
    [[nodiscard]] float pitch() const; // radians
    [[nodiscard]] float flySpeed() const;
    [[nodiscard]] glm::mat4 view() const;
    [[nodiscard]] float verticalFovDegrees() const;

private:
    [[nodiscard]] glm::vec3 right() const;
    [[nodiscard]] glm::vec3 target() const;
    void applyLook(glm::vec2 mouseDelta, const InputSettings& settings);
    void updateFly(const InputFrame& input, const InputSettings& settings, float deltaSeconds);
    void updateOrbit(const InputFrame& input, const InputSettings& settings);

    CameraMode mode_ = CameraMode::Fly;
    glm::vec3 position_{0.0f, 0.0f, 0.0f};
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
    float orbitDistance_ = 1.0f;
    float flySpeed_ = 4.0f;
    float verticalFovDegrees_ = 60.0f;
};

} // namespace blue
