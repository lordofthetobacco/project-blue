#ifndef BLUE_PLAYER_PLAYER_CONTROLLER_H_
#define BLUE_PLAYER_PLAYER_CONTROLLER_H_

#include <memory>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "core/result.h"
#include "input/input_frame.h"
#include "input/input_settings.h"
#include "physics/character.h"
#include "physics/physics_world.h"
#include "player/fov_controller.h"
#include "player/head_bob.h"
#include "scene/scene.h"

namespace blue::player {

// Gameplay constants of the player that are not user settings.
struct PlayerTuning {
  float walk_speed = 4.0f;    // m/s
  float sprint_speed = 7.0f;  // m/s
  float crouch_speed = 2.0f;  // m/s
  float aim_speed = 2.5f;     // m/s while aiming down the sights
  float jump_speed = 5.0f;    // m/s upwards, about 1.3 m under 9.81 m/s^2
  float eye_height_standing = 1.65f;   // m above the feet
  float eye_height_crouching = 1.05f;  // m above the feet
  float stance_blend_rate = 10.0f;     // 1/s, eye height when crouching
  float fov_blend_rate = 8.0f;         // 1/s
  // Fully aimed vertical FOV as a fraction of the base one: iron sights only
  // magnify a little.
  float aim_fov_scale = 0.8f;
  // 1/s, exponential approach of the aim FOV; about 95% in 0.2 s, matching
  // the rifle's raise to the shoulder.
  float aim_blend_rate = 15.0f;
  HeadBobParams head_bob;
};

// What the renderer needs from the player: eye placement, orientation and
// field of view.
struct PlayerView {
  glm::vec3 eye_position{0.0f};
  camera::LookAngles look;
  float roll = 0.0f;  // radians, head-bob sway
  float vertical_fov_degrees = 70.0f;

  [[nodiscard]] glm::vec3 Forward() const { return camera::ForwardFrom(look); }
  // Returns the world-to-view matrix including the roll.
  [[nodiscard]] glm::mat4 View() const;
  // Returns the eye and look direction as a scene camera carrying
  // `base_fov_degrees` rather than the momentary (sprint-boosted) one.
  [[nodiscard]] scene::SceneCamera ToSceneCamera(float base_fov_degrees) const;
};

// First-person controller: turns input actions into character movement,
// stance and jumps, and composes the camera view (eye height, head bob, FOV)
// from the simulated character. Owns its physics::Character, so it must be
// destroyed before the PhysicsWorld it was spawned in. Move-only. Not
// thread-safe.
class PlayerController {
 public:
  PlayerController() = default;
  PlayerController(PlayerController&&) = default;
  PlayerController& operator=(PlayerController&&) = default;

  // Creates the character standing with its feet at `feet`, looking along
  // `look`. Replaces any previous character.
  [[nodiscard]] core::Result<void, physics::PhysicsError> Spawn(
      physics::PhysicsWorld& world, const physics::CharacterDesc& desc,
      glm::vec3 feet, camera::LookAngles look);
  [[nodiscard]] bool spawned() const { return character_ != nullptr; }
  // Drops the character; the controller becomes inert until the next Spawn.
  void Despawn();

  void Teleport(glm::vec3 feet, camera::LookAngles look);
  // Call after the world's bodies were rebuilt under the character.
  void OnWorldRebuilt();

  // Applies one frame of input: mouse look, stance (hold or toggle per
  // `settings`), aiming, desired velocity and jump requests. Call before
  // PhysicsWorld::Update. An empty frame keeps the player standing still.
  //
  // Holding kAim aims down the sights: it narrows the field of view, slows
  // the player to `aim_speed`, cancels and blocks sprinting, and scales the
  // mouse sensitivity with the zoom so the on-screen turn rate is unchanged.
  void Update(const input::InputFrame& input,
              const input::InputSettings& settings, float dt);
  // Composes the view from the simulated character. Call after
  // PhysicsWorld::Update.
  void UpdateView(const input::InputSettings& settings, float dt);

  // Multiplies the mouse look of the following Updates by `factor`, on top
  // of the aim zoom's scaling (e.g. 1 / M while looking through a scope of
  // magnification M, so its picture moves at a controllable speed). Stays
  // until changed; 1 (the start value) is the plain look. Non-positive or
  // non-finite values count as 1.
  void SetLookScale(float factor);
  [[nodiscard]] float look_scale() const { return look_scale_; }

  // Turns the look by a recoil kick: `pitch_radians` up, `yaw_radians` left
  // (the LookAngles convention). The pitch stays within the look clamp. Shows
  // in the view from the next UpdateView on and is not part of look_delta().
  void AddLookKick(float pitch_radians, float yaw_radians);

  [[nodiscard]] const PlayerView& view() const { return view_; }
  [[nodiscard]] camera::LookAngles look() const { return look_; }
  [[nodiscard]] bool sprinting() const { return sprinting_; }
  // The aim input was held during the last Update.
  [[nodiscard]] bool aiming() const { return aiming_; }
  // How far the aim zoom has blended in: 0 at the hip, 1 fully aimed.
  [[nodiscard]] float aim_fraction() const { return aim_fraction_; }
  // How far the mouse turned the look during the last Update, in radians:
  // x = yaw (positive left), y = pitch (positive up). Zero without look
  // input. For weapon sway.
  [[nodiscard]] glm::vec2 look_delta() const { return look_delta_; }
  [[nodiscard]] bool crouching() const;
  [[nodiscard]] bool on_ground() const;
  [[nodiscard]] glm::vec3 velocity() const;
  [[nodiscard]] glm::vec3 feet_position() const;
  [[nodiscard]] float horizontal_speed() const;

  [[nodiscard]] const PlayerTuning& tuning() const { return tuning_; }
  void set_tuning(const PlayerTuning& tuning) { tuning_ = tuning; }

 private:
  void ResetView();
  // Returns the factor that keeps the on-screen turn rate of a mouse count
  // constant under the current aim zoom: 1 at the hip, below 1 when aimed.
  [[nodiscard]] float SensitivityScale() const;

  std::unique_ptr<physics::Character> character_;
  PlayerTuning tuning_;
  camera::LookAngles look_;
  glm::vec2 look_delta_{0.0f};
  bool sprint_latched_ = false;  // toggle mode: sprint requested
  bool crouch_latched_ = false;  // toggle mode: crouch requested
  bool sprinting_ = false;
  bool aiming_ = false;
  float aim_fraction_ = 0.0f;  // Smoothed aiming_, drives the FOV zoom.
  float look_scale_ = 1.0f;    // SetLookScale()
  float eye_height_ = 1.65f;
  HeadBob head_bob_;
  FovController fov_;
  PlayerView view_;
};

}  // namespace blue::player

#endif  // BLUE_PLAYER_PLAYER_CONTROLLER_H_
