#ifndef BLUE_PLAYER_HEAD_BOB_H_
#define BLUE_PLAYER_HEAD_BOB_H_

#include <glm/vec3.hpp>

namespace blue::player {

enum class Gait { kWalk, kSprint, kCrouch };

// Shape of the head bob. Amplitudes are in metres at `reference_speed` and
// full intensity; they scale with speed up to `max_speed_scale` times.
struct HeadBobParams {
  float walk_frequency_hz = 1.8f;    // step cycles per second
  float sprint_frequency_hz = 2.6f;
  float crouch_frequency_hz = 1.2f;
  float vertical_amplitude = 0.035f;  // m, one bounce per step
  float lateral_amplitude = 0.02f;    // m, one sway per two steps
  float roll_degrees = 0.6f;          // sways with the lateral offset
  float reference_speed = 4.0f;       // m/s
  float max_speed_scale = 1.5f;
  float fade_rate = 8.0f;  // 1/s, blend in/out when starting, stopping, jumping
  float min_speed = 0.1f;  // m/s below which the player counts as standing
};

struct HeadBobInput {
  float horizontal_speed = 0.0f;  // m/s
  Gait gait = Gait::kWalk;
  bool grounded = true;
  float intensity = 1.0f;  // [0, 1]; 0 disables the effect entirely
  float dt = 0.0f;         // seconds
};

struct HeadBobOffset {
  glm::vec3 local_offset{0.0f};  // camera-local: x right, y up, z unused
  float roll_radians = 0.0f;     // positive rolls the view clockwise
};

// Rhythmic eye offset driven by walking speed. Pure math, deterministic for a
// given sequence of inputs; call Update once per frame. Copyable.
class HeadBob {
 public:
  HeadBob() = default;
  explicit HeadBob(const HeadBobParams& params) : params_(params) {}

  [[nodiscard]] HeadBobOffset Update(const HeadBobInput& input);
  // Returns to rest instantly.
  void Reset();

  [[nodiscard]] float phase() const { return phase_; }  // radians, monotonic
  [[nodiscard]] const HeadBobParams& params() const { return params_; }

 private:
  HeadBobParams params_;
  float phase_ = 0.0f;
  float weight_ = 0.0f;  // [0, 1] blend between rest and full bob
};

}  // namespace blue::player

#endif  // BLUE_PLAYER_HEAD_BOB_H_
