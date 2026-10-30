#ifndef BLUE_CAMERA_LOOK_H_
#define BLUE_CAMERA_LOOK_H_

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "input/input_settings.h"

namespace blue::camera {

inline constexpr glm::vec3 kWorldUp{0.0f, 1.0f, 0.0f};

// Yaw/pitch view orientation shared by the debug camera and the player. Yaw 0
// looks down -Z and grows turning left; pitch is positive looking up and is
// kept within +-89 degrees.
struct LookAngles {
  float yaw = 0.0f;    // radians, in (-pi, pi]
  float pitch = 0.0f;  // radians

  friend bool operator==(const LookAngles&, const LookAngles&) = default;
};

// Returns `yaw` wrapped into (-pi, pi].
[[nodiscard]] float WrapYaw(float yaw);

// Returns the unit view direction of `look`.
[[nodiscard]] glm::vec3 ForwardFrom(LookAngles look);

// Returns the unit vector pointing right of `yaw` on the ground plane.
[[nodiscard]] glm::vec3 FlatRightFrom(float yaw);

// Returns the unit vector `yaw` points at on the ground plane.
[[nodiscard]] glm::vec3 FlatForwardFrom(float yaw);

// Turns `look` by a raw mouse `delta` (device counts, +y down) scaled by the
// sensitivity in `settings`. Deliberately not scaled by frame time: the same
// hand movement turns the view by the same angle at any frame rate.
[[nodiscard]] LookAngles ApplyMouseLook(LookAngles look, glm::vec2 delta,
                                        const input::InputSettings& settings);

// Returns the angles that look from `from` towards `to`, or the default
// (down -Z) when the points coincide or are not finite.
[[nodiscard]] LookAngles LookTowards(glm::vec3 from, glm::vec3 to);

}  // namespace blue::camera

#endif  // BLUE_CAMERA_LOOK_H_
