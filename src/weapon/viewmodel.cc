#include "weapon/viewmodel.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/vec4.hpp>

namespace blue::weapon {
namespace {

constexpr float kSnapDistance = 1.0e-4f;  // blends this close jump to target
constexpr float kMinStepSpeed = 0.1f;     // m/s below which there is no bob
constexpr float kBobFadeRate = 8.0f;      // 1/s
constexpr float kMaxBobSpeedScale = 1.5f;
// -ln(0.05): an exponential blend covers 95 % of the way in its time.
constexpr float kBlendTimeConstants = 3.0f;
constexpr uint32_t kDefaultSeed = 0x9e3779b9u;
// Share of the reload pose's tilt held at the start and end of a reload; the
// rest swells in towards the middle, around the magazine change.
constexpr float kReloadTiltBase = 0.7f;

// Moves `value` towards `target` by the share of the way an exponential
// approach at `rate` (1/s) covers in `dt`, snapping when close. Monotonic and
// frame-rate independent.
float Approach(float value, float target, float rate, float dt) {
  value += (target - value) * (1.0f - std::exp(-rate * dt));
  return std::abs(target - value) < kSnapDistance ? target : value;
}

// Advances a critically damped spring at rest at 0 with angular frequency
// `omega` by `dt`, exactly: x(t) = (x0 + (v0 + omega x0) t) e^(-omega t).
void StepSpring(float omega, float dt, float& position, float& velocity) {
  const float decay = std::exp(-omega * dt);
  const float b = velocity + omega * position;
  position = (position + b * dt) * decay;
  velocity = (velocity - omega * b * dt) * decay;
}

// Returns smoothstep(t) for t in [0, 1]: zero slope at both ends.
float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

WeaponPose Mix(const WeaponPose& a, const WeaponPose& b, float t) {
  return {
      .optic_offset = a.optic_offset + (b.optic_offset - a.optic_offset) * t,
      .rotation_radians =
          a.rotation_radians + (b.rotation_radians - a.rotation_radians) * t,
  };
}

// Returns the camera-space rotation for (pitch up, yaw left, roll clockwise).
glm::quat PoseRotation(glm::vec3 radians) {
  return glm::angleAxis(radians.y, glm::vec3(0.0f, 1.0f, 0.0f)) *
         glm::angleAxis(radians.x, glm::vec3(1.0f, 0.0f, 0.0f)) *
         glm::angleAxis(radians.z, glm::vec3(0.0f, 0.0f, -1.0f));
}

// Returns the camera-to-world transform of an eye at `position` looking
// along `look` with `roll`, matching player::PlayerView::View().
glm::mat4 CameraToWorld(glm::vec3 position, camera::LookAngles look,
                        float roll) {
  const glm::vec3 forward = camera::ForwardFrom(look);
  const glm::vec3 right = glm::normalize(glm::cross(forward, camera::kWorldUp));
  const glm::vec3 up = glm::cross(right, forward);
  const glm::vec3 rolled_up = std::cos(roll) * up + std::sin(roll) * right;
  const glm::vec3 rolled_right = glm::cross(forward, rolled_up);
  glm::mat4 transform(1.0f);
  transform[0] = glm::vec4(rolled_right, 0.0f);
  transform[1] = glm::vec4(rolled_up, 0.0f);
  transform[2] = glm::vec4(-forward, 0.0f);
  transform[3] = glm::vec4(position, 1.0f);
  return transform;
}

// Returns tan of the angle above the line of sight at which a drag-free bullet
// leaving `height` metres below the line and `range` metres short of the
// zero crosses the line at the zero; `speed` m/s, `gravity` m/s^2. Solves
// -h + L t - k (1 + t^2) = 0 with k = g L^2 / (2 v^2), smaller root.
float ZeroTangent(float height, float range, float speed, float gravity) {
  if (!(range > 0.0f)) {
    return 0.0f;
  }
  const float k =
      speed > 0.0f ? 0.5f * gravity * range * range / (speed * speed) : 0.0f;
  if (k <= 1.0e-9f) {
    return height / range;
  }
  const float discriminant = range * range - 4.0f * k * (height + k);
  if (discriminant < 0.0f) {
    return range / (2.0f * k);  // out of reach: the 45 degree-ish maximum
  }
  // Stable form of the smaller root (L - sqrt(D)) / 2k: no cancellation.
  return 2.0f * (height + k) / (range + std::sqrt(discriminant));
}

}  // namespace

Viewmodel::Viewmodel(const RifleGeometry& geometry,
                     const ViewmodelTuning& tuning)
    : geometry_(geometry),
      tuning_(tuning),
      rng_state_(tuning.seed != 0 ? tuning.seed : kDefaultSeed) {
  // Orthonormal frame of the optic axis in model space; right is model -X.
  const glm::vec3 sight = geometry_.optic_axis;
  const glm::vec3 forward = glm::length(sight) > 0.0f
                                ? glm::normalize(sight)
                                : glm::vec3(0.0f, 0.0f, 1.0f);
  const glm::vec3 model_up(0.0f, 1.0f, 0.0f);
  const glm::vec3 up =
      glm::normalize(model_up - glm::dot(model_up, forward) * forward);
  const glm::vec3 right = glm::cross(forward, up);
  const glm::mat3 model_frame(right, up, forward);
  const glm::mat3 camera_frame(glm::vec3(1.0f, 0.0f, 0.0f),
                               glm::vec3(0.0f, 1.0f, 0.0f),
                               glm::vec3(0.0f, 0.0f, -1.0f));
  aligned_ = glm::quat_cast(camera_frame * glm::transpose(model_frame));

  // Where the muzzle sits relative to the optic axis when aimed.
  const glm::vec3 muzzle =
      aligned_ *
      ((geometry_.muzzle - geometry_.optic_eye_side) * geometry_.scale);
  const float height = -muzzle.y;
  const float ahead = tuning_.eye_relief - muzzle.z;
  zero_angle_ =
      std::atan(ZeroTangent(height, tuning_.zero_distance - ahead,
                            tuning_.muzzle_velocity, tuning_.gravity));
  bore_direction_model_ =
      std::cos(zero_angle_) * forward + std::sin(zero_angle_) * up;
  Update(ViewmodelInput{});
}

void Viewmodel::Update(const ViewmodelInput& input) {
  const float dt = std::max(input.dt, 0.0f);
  const bool can_aim = input.aim_held && !input.sprinting && !input.reloading;
  const float aim_rate =
      kBlendTimeConstants / std::max(tuning_.aim_time, 1.0e-3f);
  aim_ = Approach(aim_, can_aim ? 1.0f : 0.0f, aim_rate, dt);
  sprint_ =
      Approach(sprint_, input.sprinting ? 1.0f : 0.0f, tuning_.pose_rate, dt);
  reload_ =
      Approach(reload_, input.reloading ? 1.0f : 0.0f, tuning_.pose_rate, dt);
  const float aim_weight = Smooth(aim_);
  const float motion_scale =
      1.0f + (tuning_.aim_motion_scale - 1.0f) * aim_weight;

  // Sway: the rifle lags behind the turn, then springs back.
  sway_ = glm::clamp(sway_ - input.look_delta * tuning_.sway_gain,
                     glm::vec2(-tuning_.max_sway), glm::vec2(tuning_.max_sway));
  StepSpring(tuning_.sway_frequency, dt, sway_.x, sway_velocity_.x);
  StepSpring(tuning_.sway_frequency, dt, sway_.y, sway_velocity_.y);

  // Bob: one dip per step, one side-to-side per two steps.
  const float speed =
      glm::length(glm::vec2(input.velocity.x, input.velocity.z));
  const bool stepping = input.on_ground && speed > kMinStepSpeed;
  bob_weight_ = Approach(bob_weight_, stepping ? 1.0f : 0.0f, kBobFadeRate, dt);
  bob_phase_ =
      std::fmod(bob_phase_ + glm::two_pi<float>() * tuning_.bob_frequency * dt,
                2.0f * glm::two_pi<float>());
  const float speed_scale =
      tuning_.bob_reference_speed > 0.0f
          ? std::min(speed / tuning_.bob_reference_speed, kMaxBobSpeedScale)
          : 0.0f;
  const float bob_scale = bob_weight_ * speed_scale * motion_scale;
  const glm::vec3 bob(
      tuning_.bob_amplitude.x * std::sin(0.5f * bob_phase_) * bob_scale,
      tuning_.bob_amplitude.y * 0.5f * (std::cos(bob_phase_) - 1.0f) *
          bob_scale,
      0.0f);

  // Recoil: each shot kicks the springs and the camera.
  const float recoil_scale =
      1.0f + (tuning_.aim_recoil_scale - 1.0f) * aim_weight;
  for (int shot = 0; shot < input.shots; ++shot) {
    recoil_back_velocity_ += tuning_.recoil_back_velocity * recoil_scale;
    recoil_pitch_velocity_ += tuning_.recoil_pitch_velocity * recoil_scale;
    look_kick_.x += tuning_.look_kick_pitch * recoil_scale;
    look_kick_.y += tuning_.look_kick_yaw * NextSpread() * recoil_scale;
  }
  StepSpring(tuning_.recoil_frequency, dt, recoil_back_, recoil_back_velocity_);
  StepSpring(tuning_.recoil_frequency, dt, recoil_pitch_,
             recoil_pitch_velocity_);

  // Compose: hip <-> aimed, then sprint and reload poses on top.
  const WeaponPose aimed{{0.0f, 0.0f, -tuning_.eye_relief}, {0.0f, 0.0f, 0.0f}};
  WeaponPose pose = Mix(tuning_.hip, aimed, aim_weight);
  pose = Mix(pose, tuning_.sprint, Smooth(sprint_));
  WeaponPose reload = tuning_.reload;
  reload.rotation_radians *=
      kReloadTiltBase +
      (1.0f - kReloadTiltBase) *
          std::sin(glm::pi<float>() *
                   std::clamp(input.reload_fraction, 0.0f, 1.0f));
  pose = Mix(pose, reload, Smooth(reload_));
  const glm::vec3 rotation =
      pose.rotation_radians + glm::vec3(sway_.y * motion_scale + recoil_pitch_,
                                        sway_.x * motion_scale, 0.0f);
  const glm::vec3 eye_side =
      pose.optic_offset + bob + glm::vec3(0.0f, 0.0f, recoil_back_);
  const glm::quat pose_rotation = PoseRotation(rotation);
  const glm::quat camera_rotation = pose_rotation * aligned_;

  const glm::mat4 camera_to_world =
      CameraToWorld(input.eye_position, input.look, input.roll);
  const glm::mat4 model_to_camera =
      glm::translate(glm::mat4(1.0f), eye_side) *
      glm::mat4_cast(camera_rotation) *
      glm::scale(glm::mat4(1.0f), glm::vec3(geometry_.scale)) *
      glm::translate(glm::mat4(1.0f), -geometry_.optic_eye_side);
  model_ = camera_to_world * model_to_camera;
  muzzle_position_ = glm::vec3(model_ * glm::vec4(geometry_.muzzle, 1.0f));
  grip_position_ = glm::vec3(model_ * glm::vec4(geometry_.grip, 1.0f));
  const glm::mat3 camera_to_world_rotation(camera_to_world);
  muzzle_direction_ = glm::normalize(camera_to_world_rotation *
                                     (camera_rotation * bore_direction_model_));

  // `aligned_` maps the optic frame onto the camera axes, so the posed lens
  // axes are the pose rotation applied to them.
  lens_center_ = glm::vec3(model_ * glm::vec4(geometry_.optic_lens, 1.0f));
  lens_right_ = glm::normalize(camera_to_world_rotation *
                               (pose_rotation * glm::vec3(1.0f, 0.0f, 0.0f)));
  lens_up_ = glm::normalize(camera_to_world_rotation *
                            (pose_rotation * glm::vec3(0.0f, 1.0f, 0.0f)));
  optic_axis_ = glm::normalize(camera_to_world_rotation *
                               (pose_rotation * glm::vec3(0.0f, 0.0f, -1.0f)));
}

glm::mat4 Viewmodel::lens_matrix() const {
  const float radius = lens_radius();
  glm::mat4 transform(1.0f);
  transform[0] = glm::vec4(lens_right_ * radius, 0.0f);
  transform[1] = glm::vec4(lens_up_ * radius, 0.0f);
  transform[2] = glm::vec4(optic_axis_, 0.0f);
  transform[3] = glm::vec4(lens_center_, 1.0f);
  return transform;
}

glm::vec2 Viewmodel::TakeLookKick() {
  const glm::vec2 kick = look_kick_;
  look_kick_ = glm::vec2(0.0f);
  return kick;
}

float Viewmodel::NextSpread() {
  // xorshift32: small, fast and deterministic per instance.
  rng_state_ ^= rng_state_ << 13;
  rng_state_ ^= rng_state_ >> 17;
  rng_state_ ^= rng_state_ << 5;
  constexpr float kToUnit = 1.0f / 4294967296.0f;
  return static_cast<float>(rng_state_) * kToUnit * 2.0f - 1.0f;
}

}  // namespace blue::weapon
