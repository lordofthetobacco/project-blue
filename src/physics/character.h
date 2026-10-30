#ifndef BLUE_PHYSICS_CHARACTER_H_
#define BLUE_PHYSICS_CHARACTER_H_

#include <memory>

#include <glm/vec3.hpp>

#include "core/result.h"
#include "physics/physics_world.h"

namespace blue::physics {

// Contact state of a character with the ground after its last step.
enum class GroundState {
  kOnGround,       // Standing on a walkable surface.
  kOnSteepGround,  // Touching a surface steeper than the max slope.
  kNotSupported,   // Touching something that does not carry the character.
  kInAir,
};

// Returns a short human-readable name for `state`. Never null.
const char* ToString(GroundState state);

// Dimensions and limits of a character capsule. SI units.
struct CharacterDesc {
  float standing_height = 1.8f;  // Total capsule height; >= crouch_height.
  float crouch_height = 1.2f;    // Total capsule height; > 2 * radius.
  float radius = 0.35f;
  float max_slope_degrees = 50.0f;  // Steeper surfaces do not support.
  float mass = 70.0f;               // kg, felt by pushed dynamic bodies.
  float max_strength = 100.0f;      // N the character can push with.
  float step_up = 0.4f;             // Highest stair step climbed.
  float stick_to_floor = 0.5f;      // Step-down distance that keeps contact.
  // Share of the desired horizontal velocity applied per step while airborne,
  // in [0, 1]; 0 keeps the launch velocity, 1 steers freely.
  float air_control = 0.3f;
};

// A kinematic player capsule that walks on the world's bodies, climbs steps,
// jumps and crouches. Other bodies do not see it (there is no rigid body
// behind it), but it pushes dynamic bodies it walks into. Positions are the
// feet: the bottom center of the capsule.
//
// The character is advanced by its PhysicsWorld inside every fixed step, so
// gameplay sets the desired velocity and jump requests at any time and they
// take effect on the next step. The character must be destroyed before the
// world. Neither copyable nor movable. Not thread-safe.
class Character {
 public:
  // Creates a standing character with its feet at `feet_position`. Fails with
  // kInvalidShape for an inconsistent `desc` and kShapeBuildFailed when the
  // physics library rejects the capsule.
  static core::Result<std::unique_ptr<Character>, PhysicsError> Create(
      PhysicsWorld& world, const CharacterDesc& desc,
      glm::vec3 feet_position);

  Character(const Character&) = delete;
  Character& operator=(const Character&) = delete;
  ~Character();

  // Sets the horizontal velocity (m/s) the character tries to move with; the
  // y component is ignored. Persists until changed.
  void SetDesiredVelocity(glm::vec3 horizontal);

  // Requests a jump with the given upward speed (m/s). The request is kept
  // for a few steps and consumed by the first one that finds the character on
  // the ground, so a slightly early press still jumps.
  void RequestJump(float vertical_speed);

  // Switches between the crouching and standing capsule. Returns whether the
  // requested stance is active afterwards: standing up fails (and the
  // character stays crouched) when something blocks the taller capsule.
  bool SetCrouching(bool crouching);

  // Moves the feet to `feet_position` instantly, clears the velocity and
  // refreshes the contacts.
  void Teleport(glm::vec3 feet_position);

  // Re-detects the surrounding bodies, e.g. after the world's bodies were
  // rebuilt.
  void RefreshContacts();

  // Feet position interpolated between the last two steps, like
  // PhysicsWorld::InterpolatedPose.
  [[nodiscard]] glm::vec3 position() const;
  // Velocity after the last step (m/s).
  [[nodiscard]] glm::vec3 velocity() const;
  [[nodiscard]] GroundState ground_state() const;
  // True when standing on walkable ground and not in the few steps right
  // after a jump launch.
  [[nodiscard]] bool IsOnGround() const;
  [[nodiscard]] bool crouching() const;
  // Height of the current capsule, m.
  [[nodiscard]] float height() const;
  [[nodiscard]] const CharacterDesc& desc() const;

 private:
  class Impl;
  explicit Character(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_CHARACTER_H_
