#ifndef BLUE_WEAPON_DROPPED_WEAPONS_H_
#define BLUE_WEAPON_DROPPED_WEAPONS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"
#include "weapon/weapon_item.h"
#include "weapon/weapon_throw.h"

namespace blue::weapon {

// A weapon lying in the world (or still flying), with the complete state it
// was dropped with.
struct DroppedWeapon {
  uint32_t id = 0;  // never 0 for a live item; unique for the owner's life
  WeaponItem weapon;
  // Its dynamic body; nullopt if the physics world refused one (logged): the
  // weapon then stays where it was planned, still drawn and picked up.
  std::optional<physics::BodyId> body;
  // The body's interpolated pose as of the last Sync(); the planned pose
  // before the first one.
  physics::Pose pose;
  glm::vec3 scale{1.0f};
};

// The weapons the player has dropped: their firearms, their dynamic bodies
// (one box per weapon definition, from its prefab's bounds; as heavy as the
// weapon's spec says; not blocking the player's character so a throw never
// shoves the player) and their poses.
// Lives in the weapon library because it is gameplay state, and draws
// nothing: the game draws each item's prefab at Matrix().
//
// Borrows `world`, which must outlive it, and adds and removes its own
// bodies only. Must be destroyed before the PhysicsRuntime. Allocates in
// SetShape() and Add() only, nothing per frame. Neither copyable
// nor movable. Not thread-safe.
class DroppedWeapons {
 public:
  explicit DroppedWeapons(physics::PhysicsWorld& world) : world_(world) {}
  DroppedWeapons(const DroppedWeapons&) = delete;
  DroppedWeapons& operator=(const DroppedWeapons&) = delete;
  ~DroppedWeapons();

  // Sets the collision box of weapons of definition `def`: `bounds` in
  // prefab space (thin axes padded to 1 cm). Bodies already in the world
  // keep theirs. Returns false (logged) if the shape cannot be built; the
  // previous one, if any, stays.
  bool SetShape(WeaponDefId def, const asset::Bounds& bounds);
  // The prefab-space box of `def`; empty before SetShape().
  [[nodiscard]] asset::Bounds bounds(WeaponDefId def) const;

  // Puts `weapon` into the world as `plan` says and returns its id. Never
  // loses the weapon: without a body (no shape for its definition, or the
  // world refuses one) it stays at the planned pose.
  uint32_t Add(WeaponItem weapon, const ThrowPlan& plan);
  // Removes item `id` and its body and returns its weapon; nullopt for an
  // unknown id.
  std::optional<WeaponItem> Take(uint32_t id);
  // Removes every item and its body.
  void Clear();

  // Reads the bodies' interpolated poses; once per frame after the physics
  // world was updated.
  void Sync();
  // Creates the bodies anew, at rest at the poses of the last Sync(), after
  // the physics world lost them (e.g. world::WorldPhysics::Build() removes
  // every body). Stale handles are removed first.
  void RecreateBodies();

  [[nodiscard]] std::span<const DroppedWeapon> items() const { return items_; }
  [[nodiscard]] const DroppedWeapon* Find(uint32_t id) const;
  [[nodiscard]] const DroppedWeapon* FindByBody(physics::BodyId body) const;

  // Returns the prefab space -> world matrix `item` is drawn with.
  [[nodiscard]] static glm::mat4 Matrix(const DroppedWeapon& item);

 private:
  // Creates `item`'s body at its pose with the given velocities.
  void CreateBody(DroppedWeapon& item, glm::vec3 linear_velocity,
                  glm::vec3 angular_velocity);

  // The box of one weapon definition.
  struct Shape {
    std::optional<physics::CookedShape> shape;
    asset::Bounds bounds;
  };

  physics::PhysicsWorld& world_;
  std::vector<Shape> shapes_;  // indexed by WeaponDefId, grown by SetShape()
  std::vector<DroppedWeapon> items_;
  uint32_t next_id_ = 1;
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_DROPPED_WEAPONS_H_
