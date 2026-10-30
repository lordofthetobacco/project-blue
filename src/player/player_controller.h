#ifndef BLUE_PLAYER_PLAYER_CONTROLLER_H_
#define BLUE_PLAYER_PLAYER_CONTROLLER_H_

#include <memory>

#include <glm/mat4x4.hpp>
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
  float jump_speed = 5.0f;    // m/s upwards, about 1.3 m under 9.81 m/s^2
  float eye_height_standing = 1.65f;   // m above the feet
  float eye_height_crouching = 1.05f;  // m above the feet
  float stance_blend_rate = 10.0f;     // 1/s, eye height when crouching
  float fov_blend_rate = 8.0f;         // 1/s
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
  // `settings`), desired velocity and jump requests. Call before
  // PhysicsWorld::Update. An empty frame keeps the player standing still.
  void Update(const input::InputFrame& input,
              const input::InputSettings& settings, float dt);
  // Composes the view from the simulated character. Call after
  // PhysicsWorld::Update.
  void UpdateView(const input::InputSettings& settings, float dt);

  [[nodiscard]] const PlayerView& view() const { return view_; }
  [[nodiscard]] camera::LookAngles look() const { return look_; }
  [[nodiscard]] bool sprinting() const { return sprinting_; }
  [[nodiscard]] bool crouching() const;
  [[nodiscard]] bool on_ground() const;
  [[nodiscard]] glm::vec3 velocity() const;
  [[nodiscard]] glm::vec3 feet_position() const;
  [[nodiscard]] float horizontal_speed() const;

  [[nodiscard]] const PlayerTuning& tuning() const { return tuning_; }
  void set_tuning(const PlayerTuning& tuning) { tuning_ = tuning; }

 private:
  void ResetView();

  std::unique_ptr<physics::Character> character_;
  PlayerTuning tuning_;
  camera::LookAngles look_;
  bool sprint_latched_ = false;  // toggle mode: sprint requested
  bool crouch_latched_ = false;  // toggle mode: crouch requested
  bool sprinting_ = false;
  float eye_height_ = 1.65f;
  HeadBob head_bob_;
  FovController fov_;
  PlayerView view_;
};

}  // namespace blue::player

#endif  // BLUE_PLAYER_PLAYER_CONTROLLER_H_
