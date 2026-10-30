#ifndef BLUE_CAMERA_CAMERA_CONTROLLER_H_
#define BLUE_CAMERA_CAMERA_CONTROLLER_H_

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "input/input_frame.h"
#include "input/input_settings.h"
#include "scene/scene.h"

namespace blue::camera {

enum class CameraMode {
  kFly,    // Free flight: keys move the camera, the mouse turns it.
  kOrbit,  // The camera swings around a pivot in front of it.
};

// Pure-math camera: free-fly and orbit share one state (position, yaw, pitch,
// orbit distance), so switching modes never moves the view. The orbit target
// is always position + forward * distance. Copyable; not thread-safe for
// concurrent mutation.
class CameraController {
 public:
  CameraController() = default;
  CameraController(const CameraController&) = default;
  CameraController& operator=(const CameraController&) = default;
  CameraController(CameraController&&) = default;
  CameraController& operator=(CameraController&&) = default;

  // Replaces the camera state with the view described by `camera`. A target
  // that coincides with the position (or is not finite) leaves the camera
  // looking down -Z with an orbit distance of 1.
  void SetFromSceneCamera(const scene::SceneCamera& camera);
  // Returns the current view as a scene camera, with the orbit target as
  // `target`.
  [[nodiscard]] scene::SceneCamera ToSceneCamera() const;

  void set_mode(CameraMode mode) { mode_ = mode; }
  [[nodiscard]] CameraMode mode() const { return mode_; }

  // Advances the camera by one frame of `input`. Fly mode moves with the keys
  // and turns with the mouse; orbit mode ignores the movement keys and
  // rotates, pans or zooms around the target. Mouse deltas are raw device
  // counts and are not scaled by `delta_seconds`; movement is.
  void Update(const input::InputFrame& input,
              const input::InputSettings& settings, float delta_seconds);

  [[nodiscard]] glm::vec3 position() const { return position_; }
  // Returns the unit view direction.
  [[nodiscard]] glm::vec3 Forward() const;
  [[nodiscard]] float yaw() const { return yaw_; }      // radians
  [[nodiscard]] float pitch() const { return pitch_; }  // radians
  [[nodiscard]] LookAngles look() const {
    return {.yaw = yaw_, .pitch = pitch_};
  }
  // Moves the camera to `position` looking along `look`; the orbit distance
  // is kept.
  void SetPose(glm::vec3 position, LookAngles look);
  [[nodiscard]] float fly_speed() const { return fly_speed_; }
  // Returns the world-to-view matrix.
  [[nodiscard]] glm::mat4 View() const;
  [[nodiscard]] float vertical_fov_degrees() const {
    return vertical_fov_degrees_;
  }

 private:
  [[nodiscard]] glm::vec3 Right() const;
  [[nodiscard]] glm::vec3 Target() const;
  void ApplyLook(glm::vec2 mouse_delta, const input::InputSettings& settings);
  void UpdateFly(const input::InputFrame& input,
                 const input::InputSettings& settings, float delta_seconds);
  void UpdateOrbit(const input::InputFrame& input,
                   const input::InputSettings& settings);

  CameraMode mode_ = CameraMode::kFly;
  glm::vec3 position_{0.0f, 0.0f, 0.0f};
  float yaw_ = 0.0f;
  float pitch_ = 0.0f;
  float orbit_distance_ = 1.0f;
  float fly_speed_ = 4.0f;
  float vertical_fov_degrees_ = 60.0f;
};

}  // namespace blue::camera

#endif  // BLUE_CAMERA_CAMERA_CONTROLLER_H_
