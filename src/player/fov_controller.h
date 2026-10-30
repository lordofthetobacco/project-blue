#ifndef BLUE_PLAYER_FOV_CONTROLLER_H_
#define BLUE_PLAYER_FOV_CONTROLLER_H_

namespace blue::player {

struct FovInput {
  float base_degrees = 70.0f;
  float sprint_boost_degrees = 0.0f;  // added to the target while sprinting
  bool sprinting = false;
  float blend_rate = 8.0f;  // 1/s, exponential approach to the target
  float dt = 0.0f;          // seconds
};

// Smoothly follows a target vertical field of view that widens while
// sprinting. The first Update snaps to the target so a loaded setting does
// not animate in. Copyable.
class FovController {
 public:
  // Returns the field of view to render with this frame, in degrees.
  [[nodiscard]] float Update(const FovInput& input);
  // Makes the next Update snap instead of blend.
  void Reset() { initialized_ = false; }

  [[nodiscard]] float current() const { return current_; }

 private:
  float current_ = 0.0f;
  bool initialized_ = false;
};

}  // namespace blue::player

#endif  // BLUE_PLAYER_FOV_CONTROLLER_H_
