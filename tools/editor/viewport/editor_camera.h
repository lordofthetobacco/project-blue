#ifndef BLUE_EDITOR_VIEWPORT_EDITOR_CAMERA_H_
#define BLUE_EDITOR_VIEWPORT_EDITOR_CAMERA_H_

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

// The editor viewport's camera. Pure math: no ImGui, no GPU.

namespace blue::editor {

// A world-space ray; `direction` is unit length.
struct Ray {
  glm::vec3 origin{0.0f};
  glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

// An orbit camera that also flies: it looks at `target()` from `distance()`
// metres along the direction given by yaw and pitch. Orbit / pan / dolly
// move it around the target (Alt+LMB, MMB, wheel); Look / Move turn it in
// place and move target and eye together (RMB + WASD). Y is up; yaw 0 looks
// down -Z. Projection: reversed-Z infinite perspective, like the game.
// Copyable value.
class EditorCamera {
 public:
  static constexpr float kMinDistance = 0.05f;
  static constexpr float kMaxDistance = 5000.0f;

  EditorCamera() = default;
  EditorCamera(const EditorCamera&) = default;
  EditorCamera& operator=(const EditorCamera&) = default;

  // Turns around the target by `delta_radians` (x: yaw, y: pitch); pitch is
  // clamped just short of straight up / down.
  void Orbit(glm::vec2 delta_radians);
  // Moves eye and target in the view plane so the point under the cursor
  // follows a mouse move of `delta_pixels` (x right, y down) in a viewport
  // `viewport_height` pixels high.
  void Pan(glm::vec2 delta_pixels, float viewport_height);
  // Moves towards the target (positive steps) or away, by 15 % of the
  // distance per step, clamped to [kMinDistance, kMaxDistance].
  void Dolly(float steps);
  // Turns in place (the eye stays, the target moves) by `delta_radians`.
  void Look(glm::vec2 delta_radians);
  // Moves eye and target by `local` metres in view space (x right, y up,
  // z forward).
  void Move(const glm::vec3& local);
  // Looks at `center` from a distance at which a sphere of `radius` fills
  // the view, keeping the direction.
  void Frame(const glm::vec3& center, float radius);
  // Turns the camera around the target to `yaw` / `pitch` (radians, pitch
  // clamped like Orbit()).
  void SetOrientation(float yaw, float pitch);
  // Places the camera at `eye` looking at `target`.
  void LookAt(const glm::vec3& eye, const glm::vec3& target);

  [[nodiscard]] glm::vec3 position() const;
  // Unit view direction.
  [[nodiscard]] glm::vec3 forward() const;
  [[nodiscard]] glm::mat4 view() const;
  [[nodiscard]] glm::mat4 projection(float aspect) const;
  // Returns the ray through `ndc` (x right, y up, both in [-1, 1]) of a
  // viewport with `aspect` = width / height.
  [[nodiscard]] Ray ScreenRay(glm::vec2 ndc, float aspect) const;

  [[nodiscard]] const glm::vec3& target() const { return target_; }
  [[nodiscard]] float distance() const { return distance_; }
  [[nodiscard]] float yaw() const { return yaw_; }
  [[nodiscard]] float pitch() const { return pitch_; }
  [[nodiscard]] float vertical_fov_radians() const { return fov_; }
  [[nodiscard]] float near_plane() const { return near_; }

 private:
  glm::vec3 target_{0.0f};
  float distance_ = 5.0f;
  float yaw_ = 0.6f;     // radians about +Y
  float pitch_ = -0.4f;  // radians, negative looks down
  float fov_ = 0.9f;     // vertical, radians (~52 degrees)
  float near_ = 0.02f;
};

}  // namespace blue::editor

#endif  // BLUE_EDITOR_VIEWPORT_EDITOR_CAMERA_H_
