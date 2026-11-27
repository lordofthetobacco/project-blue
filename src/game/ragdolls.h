#ifndef BLUE_GAME_RAGDOLLS_H_
#define BLUE_GAME_RAGDOLLS_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "animation/transform.h"
#include "game/character_animator.h"
#include "physics/physics_world.h"
#include "prefab/prefab.h"

// Ragdolls of killed characters: a cosmetic, locally simulated copy of the
// body that takes over the animated pose at death, reacts to the bullet that
// hit it, collides with the world and settles. Never replicated; the server
// does not know about them.
//
// A ragdoll is simulated for a few seconds, then frozen in its last pose
// (its physics bodies are removed, the pose is kept for drawing) and finally
// removed after it lingered for a while. At most `max_ragdolls` exist; a new
// one evicts the oldest.

namespace blue::game {

// Handle of one ragdoll. 0 = none; ids are never reused.
using RagdollId = uint32_t;

// Where on the body a bullet hit, when the exact point is unknown.
enum class RagdollZone : uint8_t { kAny, kHead, kTorso, kLegs };

// The bullet that killed (or hit) the character.
struct RagdollHit {
  glm::vec3 point{0.0f};     // World; on or near the body.
  glm::vec3 velocity{0.0f};  // Bullet velocity at the hit, m/s.
  float mass = 0.004f;       // kg
  RagdollZone zone = RagdollZone::kAny;
};

// Input of Ragdolls::Spawn.
struct RagdollSpawn {
  glm::mat4 root{1.0f};                          // CharacterAnimator::root()
  std::span<const animation::Transform> locals;  // CharacterAnimator::locals()
  glm::vec3 velocity{0.0f};                      // Of the body, m/s.
  std::optional<RagdollHit> hit;
  uint64_t owner = 0;  // Caller's tag (e.g. the victim's player id).
};

struct RagdollTuning {
  int max_ragdolls = 8;
  float linger_seconds = 30.0f;         // Until removal, from the spawn.
  float max_simulated_seconds = 10.0f;  // Until frozen at the latest.
  float settle_speed = 0.12f;           // m/s: slower counts as at rest.
  float settle_seconds = 1.0f;          // At rest this long: frozen.
  float impulse_scale = 15.0f;          // Times bullet mass * speed.
  float min_impulse = 30.0f;            // N*s
  float max_impulse = 100.0f;           // N*s
  float hit_ray_back = 1.0f;            // m: hit ray starts this far back.
  float hit_ray_length = 2.0f;          // m
};

// One ragdoll as the game draws it.
struct RagdollView {
  RagdollId id = 0;
  uint64_t owner = 0;
  bool simulated = false;  // False once frozen.
  glm::vec3 center{0.0f};  // Pelvis position, world.
  glm::mat4 head{1.0f};    // Head node, world (death camera).
  // Draw with CharacterModel::AppendDraws and an IDENTITY root.
  std::span<const animation::Transform> locals;
};

// Manages the ragdolls of one PhysicsWorld. Borrows the world, which must
// outlive it. Not copyable. Not thread-safe.
class Ragdolls {
 public:
  explicit Ragdolls(physics::PhysicsWorld& world,
                    const RagdollTuning& tuning = {});
  Ragdolls(const Ragdolls&) = delete;
  Ragdolls& operator=(const Ragdolls&) = delete;
  ~Ragdolls();

  // Builds the ragdoll skeleton of the player model. Returns false (and
  // keeps no model) when the model cannot be ragdolled. Existing ragdolls
  // are cleared.
  bool SetModel(const CharacterRig& rig, const prefab::Prefab& prefab);
  void ClearModel();
  [[nodiscard]] bool has_model() const;

  // Spawns a ragdoll in the pose of `spawn`. Returns 0 when there is no
  // model or the physics rejects it.
  RagdollId Spawn(const RagdollSpawn& spawn);
  // Advances freezing and removal by `dt` seconds and refreshes views();
  // call after the physics step.
  void Update(float dt);
  // Returns whether `id` existed.
  bool Remove(RagdollId id);
  void Clear();

  // Every live ragdoll, oldest first. Valid until the next call that
  // changes the set (Spawn, Update, Remove, Clear, SetModel).
  [[nodiscard]] std::span<const RagdollView> views() const;
  [[nodiscard]] const RagdollView* Find(RagdollId id) const;
  // The ragdoll that owns physics body `body`, or 0.
  [[nodiscard]] RagdollId FindByBody(physics::BodyId body) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::game

#endif  // BLUE_GAME_RAGDOLLS_H_
