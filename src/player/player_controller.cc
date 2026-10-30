#include "player/player_controller.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "input/action.h"

namespace blue::player {
namespace {

using input::Action;

[[nodiscard]] float Axis(bool positive, bool negative) {
  return (positive ? 1.0f : 0.0f) - (negative ? 1.0f : 0.0f);
}

}  // namespace

glm::mat4 PlayerView::View() const {
  const glm::vec3 forward = Forward();
  const glm::vec3 right = glm::normalize(glm::cross(forward, camera::kWorldUp));
  const glm::vec3 up = glm::cross(right, forward);
  const glm::vec3 rolled_up = std::cos(roll) * up + std::sin(roll) * right;
  return glm::lookAt(eye_position, eye_position + forward, rolled_up);
}

scene::SceneCamera PlayerView::ToSceneCamera(float base_fov_degrees) const {
  return {
      .position = eye_position,
      .target = eye_position + Forward(),
      .vertical_fov_degrees = base_fov_degrees,
  };
}

core::Result<void, physics::PhysicsError> PlayerController::Spawn(
    physics::PhysicsWorld& world, const physics::CharacterDesc& desc,
    glm::vec3 feet, camera::LookAngles look) {
  using Result = core::Result<void, physics::PhysicsError>;
  auto created = physics::Character::Create(world, desc, feet);
  if (!created.ok()) {
    return Result::Error(created.error());
  }
  character_ = std::move(created).value();
  look_ = look;
  ResetView();
  return Result::Ok();
}

void PlayerController::Despawn() {
  character_.reset();
  sprinting_ = false;
}

void PlayerController::Teleport(glm::vec3 feet, camera::LookAngles look) {
  look_ = look;
  if (character_) {
    character_->Teleport(feet);
  }
  ResetView();
}

void PlayerController::OnWorldRebuilt() {
  if (character_) {
    character_->RefreshContacts();
  }
}

void PlayerController::Update(const input::InputFrame& input,
                              const input::InputSettings& settings,
                              float /*dt*/) {
  if (!character_) {
    return;
  }
  const input::ActionState& actions = input.actions;

  if (input.look) {
    look_ = camera::ApplyMouseLook(look_, input.mouse_delta, settings);
  }

  const float move_x =
      Axis(actions.held(Action::kMoveRight), actions.held(Action::kMoveLeft));
  const float move_z =
      Axis(actions.held(Action::kMoveForward), actions.held(Action::kMoveBack));
  const bool moving = move_x != 0.0f || move_z != 0.0f;

  // Stance. In toggle mode the latch follows the request, not the result, so
  // standing up blocked by a ceiling is retried every frame until it fits.
  bool want_crouch = actions.held(Action::kCrouch);
  if (settings.crouch_toggle) {
    if (actions.pressed(Action::kCrouch)) {
      crouch_latched_ = !crouch_latched_;
    }
    want_crouch = crouch_latched_;
  }
  if (want_crouch != character_->crouching()) {
    (void)character_->SetCrouching(want_crouch);
  }
  const bool crouching = character_->crouching();

  bool sprint_requested = actions.held(Action::kSprint);
  if (settings.sprint_toggle) {
    if (actions.pressed(Action::kSprint)) {
      sprint_latched_ = !sprint_latched_;
    }
    if (!moving) {
      sprint_latched_ = false;  // Letting go of the keys ends a sprint.
    }
    sprint_requested = sprint_latched_;
  }
  sprinting_ = sprint_requested && moving && !crouching;

  const float speed = crouching    ? tuning_.crouch_speed
                      : sprinting_ ? tuning_.sprint_speed
                                   : tuning_.walk_speed;
  glm::vec3 direction = camera::FlatForwardFrom(look_.yaw) * move_z +
                        camera::FlatRightFrom(look_.yaw) * move_x;
  const float length = glm::length(direction);
  if (length > 1.0f) {
    direction /= length;
  }
  character_->SetDesiredVelocity(direction * speed);

  if (actions.pressed(Action::kJump) && !crouching) {
    character_->RequestJump(tuning_.jump_speed);
  }
}

void PlayerController::UpdateView(const input::InputSettings& settings,
                                  float dt) {
  if (!character_) {
    return;
  }
  const bool crouching = character_->crouching();
  const float target_eye = crouching ? tuning_.eye_height_crouching
                                     : tuning_.eye_height_standing;
  eye_height_ += (target_eye - eye_height_) *
                 (1.0f - std::exp(-tuning_.stance_blend_rate * dt));

  const Gait gait = crouching    ? Gait::kCrouch
                    : sprinting_ ? Gait::kSprint
                                 : Gait::kWalk;
  const HeadBobOffset bob = head_bob_.Update({
      .horizontal_speed = horizontal_speed(),
      .gait = gait,
      .grounded = character_->IsOnGround(),
      .intensity = settings.head_bob_intensity,
      .dt = dt,
  });
  const float fov = fov_.Update({
      .base_degrees = settings.fov_degrees,
      .sprint_boost_degrees = settings.sprint_fov_boost_degrees,
      .sprinting = sprinting_,
      .blend_rate = tuning_.fov_blend_rate,
      .dt = dt,
  });

  const glm::vec3 right = camera::FlatRightFrom(look_.yaw);
  view_ = {
      .eye_position = character_->position() +
                      camera::kWorldUp * (eye_height_ + bob.local_offset.y) +
                      right * bob.local_offset.x,
      .look = look_,
      .roll = bob.roll_radians,
      .vertical_fov_degrees = fov,
  };
}

bool PlayerController::crouching() const {
  return character_ && character_->crouching();
}

bool PlayerController::on_ground() const {
  return character_ && character_->IsOnGround();
}

glm::vec3 PlayerController::velocity() const {
  return character_ ? character_->velocity() : glm::vec3(0.0f);
}

glm::vec3 PlayerController::feet_position() const {
  return character_ ? character_->position() : glm::vec3(0.0f);
}

float PlayerController::horizontal_speed() const {
  const glm::vec3 v = velocity();
  return std::sqrt(v.x * v.x + v.z * v.z);
}

void PlayerController::ResetView() {
  sprint_latched_ = false;
  crouch_latched_ = false;
  sprinting_ = false;
  eye_height_ = tuning_.eye_height_standing;
  head_bob_.Reset();
  fov_.Reset();
  view_ = {
      .eye_position = feet_position() + camera::kWorldUp * eye_height_,
      .look = look_,
      .roll = 0.0f,
      .vertical_fov_degrees = view_.vertical_fov_degrees,
  };
}

}  // namespace blue::player
