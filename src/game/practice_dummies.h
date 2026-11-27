#ifndef BLUE_GAME_PRACTICE_DUMMIES_H_
#define BLUE_GAME_PRACTICE_DUMMIES_H_

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "game/character_animator.h"
#include "game/ragdolls.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"

// The offline practice dummies: the Kyle figures of the shooting range turned
// into targets. A standing dummy is an animated character with a static
// capsule proxy body the bullets and the player collide with. A bullet that
// hits the proxy "kills" the dummy: the proxy goes, the caller gets a
// RagdollSpawn describing the body and the impact (to hand to
// game::Ragdolls), and after a while the dummy stands up again.
//
// GPU-free: it keeps poses and bodies, the app draws them.

namespace blue::game {

struct PracticeDummyTuning {
  float radius = 0.35f;          // m, proxy capsule
  float height = 1.8f;           // m, proxy capsule, feet to top
  float respawn_seconds = 5.0f;  // From the kill until the dummy stands again.
};

// Where one dummy stands.
struct DummyPlacement {
  glm::vec3 feet{0.0f};
  float yaw = 0.0f;  // camera::LookAngles convention
  WeaponHold hold;   // How it grips its weapon; kNone for empty hands.
  // Opaque to this class: lets the caller find what to draw in the dummy's
  // hands.
  uint32_t tag = 0;
};

// One dummy as the game draws it.
struct PracticeDummy {
  DummyPlacement placement;
  bool standing = true;  // False while down (a ragdoll lies in its place).
  float down_seconds = 0.0f;
  std::optional<physics::BodyId> proxy;  // Set while standing.
  std::optional<CharacterAnimator> animator;
};

// Not copyable. Borrows the world, which must outlive it. Not thread-safe.
class PracticeDummies {
 public:
  explicit PracticeDummies(physics::PhysicsWorld& world,
                           const PracticeDummyTuning& tuning = {});
  PracticeDummies(const PracticeDummies&) = delete;
  PracticeDummies& operator=(const PracticeDummies&) = delete;
  ~PracticeDummies();

  // Replaces the dummies with one per entry of `placements`, standing and
  // animated with `rig`. A dummy whose proxy cannot be created is left out.
  // Returns how many were made.
  int Reset(const CharacterRig& rig,
            std::span<const DummyPlacement> placements);
  // Removes every dummy and the bodies of the standing ones. Call before the
  // physics world drops its bodies.
  void Clear();

  // A bullet with `velocity` (m/s) and `mass` (kg) hit physics body `body`
  // at `point`. When that is a standing dummy's proxy the dummy goes down:
  // returns the ragdoll to spawn for it (hit zone from the height of
  // `point`; `locals` stays valid until the next Update, Reset or Clear).
  // Any other body: nullopt, nothing changes.
  [[nodiscard]] std::optional<RagdollSpawn> Hit(physics::BodyId body,
                                                glm::vec3 point,
                                                glm::vec3 velocity, float mass);

  // Animates the standing dummies (idle) and stands the fallen ones up again
  // once their time is over.
  void Update(float dt);

  // Every dummy, in placement order. Valid until Reset or Clear.
  [[nodiscard]] std::span<const PracticeDummy> dummies() const {
    return dummies_;
  }
  [[nodiscard]] bool IsProxy(physics::BodyId body) const;

 private:
  // Creates the proxy of `dummy`; false when the shape or body fails.
  bool MakeProxy(PracticeDummy& dummy);
  void Animate(PracticeDummy& dummy, float dt);

  physics::PhysicsWorld& world_;
  PracticeDummyTuning tuning_;
  std::optional<physics::CookedShape> proxy_shape_;
  std::vector<PracticeDummy> dummies_;
};

}  // namespace blue::game

#endif  // BLUE_GAME_PRACTICE_DUMMIES_H_
