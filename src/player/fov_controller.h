#ifndef BLUE_PLAYER_FOV_CONTROLLER_H_
#define BLUE_PLAYER_FOV_CONTROLLER_H_

namespace blue::player {

// Returns the vertical field of view, in degrees, that magnifies a view of
// `base_degrees` by `magnification`: 2 atan(tan(base / 2) / magnification),
// so the picture's linear size grows by exactly that factor. A
// magnification below 1 counts as 1.
[[nodiscard]] float MagnifiedFovDegrees(float base_degrees,
                                        float magnification);

// Returns the factor on the look sensitivity that keeps the on-screen speed
// of a mouse count the same at `fov_degrees` as at `base_fov_degrees`:
// tan(fov / 2) / tan(base / 2). 1 when the base is not a usable angle.
[[nodiscard]] float ZoomSensitivityScale(float fov_degrees,
                                         float base_fov_degrees);

struct FovInput {
  float base_degrees = 70.0f;
  float sprint_boost_degrees = 0.0f;  // added to the target while sprinting
  bool sprinting = false;
  float blend_rate = 8.0f;  // 1/s, exponential approach to the target
  float dt = 0.0f;          // seconds
  // Multiplies the smoothed result, e.g. 0.8 when aiming down the sights.
  // Applied after the blend and the clamp: the caller smooths it itself.
  float zoom = 1.0f;
};

// Smoothly follows a target vertical field of view that widens while
// sprinting. The first Update snaps to the target so a loaded setting does
// not animate in. Copyable.
class FovController {
 public:
  // Returns the field of view to render with this frame, in degrees: the
  // smoothed target times `input.zoom`.
  [[nodiscard]] float Update(const FovInput& input);
  // Makes the next Update snap instead of blend.
  void Reset() { initialized_ = false; }

  // The smoothed field of view before the zoom, in degrees.
  [[nodiscard]] float current() const { return current_; }

 private:
  float current_ = 0.0f;
  bool initialized_ = false;
};

}  // namespace blue::player

#endif  // BLUE_PLAYER_FOV_CONTROLLER_H_
