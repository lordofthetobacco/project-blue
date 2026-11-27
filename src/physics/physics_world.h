#ifndef BLUE_PHYSICS_PHYSICS_WORLD_H_
#define BLUE_PHYSICS_PHYSICS_WORLD_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "core/result.h"
#include "scene/scene.h"

namespace blue::physics {

class CookedShape;
class PhysicsRuntime;

// Length of one simulation step in seconds: the world advances in steps of
// exactly this size, and the multiplayer tick matches it.
inline constexpr float kFixedStepSeconds = 1.0f / 60.0f;

// Handle of a body inside one PhysicsWorld. Handles of removed bodies are
// detected as stale (operations on them are ignored), but a handle is only
// meaningful for the world that created it.
enum class BodyId : uint32_t {};

enum class PhysicsError {
  kInvalidShape,         // Bad body description, bounds, scale or mesh data.
  kDynamicTriangleMesh,  // Triangle meshes can only be static.
  kTooManyBodies,        // The world's body capacity is exhausted.
  kShapeBuildFailed,     // The physics library rejected the shape.
};

// Returns a short human-readable name for `error`. Never null.
const char* ToString(PhysicsError error);

struct Pose {
  glm::vec3 position{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};  // identity (w, x, y, z)
};

// Everything needed to create one body. `mesh` is borrowed for the duration of
// CreateBody only and must be non-null for kConvexHull and kTriangleMesh.
// `shape_key` identifies the source mesh so equal shapes are built once; an
// empty key disables sharing for that body.
struct BodyDesc {
  scene::RigidBody body;
  Pose pose;
  glm::vec3 scale{1.0f};
  asset::Bounds bounds;  // Local-space bounds of the mesh (box/sphere sizing).
  const asset::MeshData* mesh = nullptr;
  std::string shape_key;  // E.g. the entity's mesh path.
};

// Everything besides the shape needed to create a body from a CookedShape
// (prefab colliders). `scale` is applied to the shape (Jolt ScaledShape);
// shapes that only support uniform scale (spheres, capsules) get the largest
// component. `mass` is used for dynamic bodies only.
struct ShapeBodyDesc {
  scene::MotionType motion = scene::MotionType::kStatic;
  Pose pose;
  glm::vec3 scale{1.0f};
  float mass = 1.0f;         // kg, > 0 for dynamic bodies
  float friction = 0.6f;     // >= 0
  float restitution = 0.1f;  // [0, 1]
  // Dynamic bodies only: the velocities they start with (m/s, rad/s).
  glm::vec3 linear_velocity{0.0f};
  glm::vec3 angular_velocity{0.0f};
  // False: characters walk through the body (it still collides with every
  // other body), so e.g. a thrown item never shoves the player.
  bool blocks_characters = true;
};

// Full motion state of a rigid body, for saving and rewinding world objects.
// `pose` is the body origin (not the center of mass).
struct BodyState {
  Pose pose;
  glm::vec3 linear_velocity{0.0f};   // m/s
  glm::vec3 angular_velocity{0.0f};  // rad/s
  bool active = false;               // Awake; sleeping bodies are skipped.
};

struct RayHit {
  BodyId body;
  glm::vec3 point;
  glm::vec3 normal;
  float fraction;  // Hit distance divided by the cast's max_distance, [0, 1].
};

// Which bodies a ray cast can hit.
enum class RayFilter : uint8_t {
  kGameplay,    // Everything but cosmetic bodies (ragdoll parts). The default.
  kEverything,  // Cosmetic bodies too.
};

struct PhysicsStats {
  int bodies;         // Bodies in the world, static ones included.
  int active_bodies;  // Dynamic bodies currently awake.
  // Wall-clock time spent stepping the simulation in the most recent Update
  // call that ran at least one step, or the most recent StepOnce.
  float last_step_ms;
};

// Construction options of a PhysicsWorld.
struct PhysicsWorldOptions {
  // Threads the simulation spreads its work over besides the calling thread.
  // Negative: one less than the hardware threads (at least 1). 0 runs every
  // job on the calling thread.
  int worker_threads = -1;
};

// A rigid-body simulation (JoltPhysics) advanced in fixed 1/60 s steps. SI
// units, right-handed, Y-up, single precision. Not thread-safe: call every
// method from one thread (the simulation itself uses worker threads
// internally). Not copyable and not movable.
class PhysicsWorld {
 public:
  // `runtime` must outlive the world; it only documents that process-wide
  // setup exists.
  explicit PhysicsWorld(const PhysicsRuntime& runtime);
  PhysicsWorld(const PhysicsRuntime& runtime,
               const PhysicsWorldOptions& options);
  PhysicsWorld(const PhysicsWorld&) = delete;
  PhysicsWorld& operator=(const PhysicsWorld&) = delete;
  ~PhysicsWorld();

  // Creates a body and returns its handle. Shapes come from the mesh bounds
  // (box, sphere) or vertices (hull, triangle mesh) scaled by `desc.scale`.
  // Axes of a box thinner than 0.1 m are thickened to 0.1 m towards -axis so a
  // flat ground plane keeps its surface as the box's top face.
  core::Result<BodyId, PhysicsError> CreateBody(const BodyDesc& desc);

  // Creates a body with the restored `shape` (shared, not copied), scaled by
  // `desc.scale`. Fails with kInvalidShape for a bad scale, pose or
  // parameter and kDynamicTriangleMesh for a dynamic triangle mesh shape.
  core::Result<BodyId, PhysicsError> CreateBody(const CookedShape& shape,
                                                const ShapeBodyDesc& desc);

  // Removes a body. Unknown or stale handles are ignored, and so are the
  // parts of a Ragdoll (they live and die with their Ragdoll handle).
  void RemoveBody(BodyId id);
  // Removes every body, including every ragdoll (constraints first). Ragdoll
  // handles outliving the call report valid() == false.
  void RemoveAllBodies();

  void SetGravity(glm::vec3 gravity);

  // Applies `force` (N) at the center of mass for the next physics step only.
  // Like Jolt, forces are cleared after a step: call this every frame to keep
  // a force acting. Wakes a sleeping body. Ignored for static bodies.
  void AddForce(BodyId id, glm::vec3 force);

  // Applies an instantaneous impulse (N*s) at the center of mass / at
  // `world_point`. Wakes a sleeping body. Ignored for static bodies.
  void AddImpulse(BodyId id, glm::vec3 impulse);
  void AddImpulseAtPoint(BodyId id, glm::vec3 impulse, glm::vec3 world_point);

  // Casts a ray from `origin` along `direction` (need not be normalized) and
  // returns the closest hit within `max_distance` meters, if any.
  // Skips cosmetic bodies (ragdoll parts), like CastRay(..., kGameplay).
  std::optional<RayHit> CastRay(glm::vec3 origin, glm::vec3 direction,
                                float max_distance) const;

  // As above, but `filter` chooses whether cosmetic bodies are hit too.
  std::optional<RayHit> CastRay(glm::vec3 origin, glm::vec3 direction,
                                float max_distance, RayFilter filter) const;

  // Advances the simulation by `frame_seconds` of real time using fixed 1/60 s
  // steps: time accumulates (at most 0.25 s per call), whole steps run, at most
  // 4 per call. If more were due the backlog is dropped rather than carried.
  // Equivalent to the right number of StepOnce calls.
  void Update(float frame_seconds);

  // Runs exactly one fixed step: every character that is not manually stepped
  // moves, then the rigid bodies. For callers that count ticks with their own
  // clock; they report the sub-step remainder with SetInterpolationAlpha.
  void StepOnce();

  // Sets the fraction of the next fixed step already elapsed, [0, 1], that
  // InterpolatedPose and Character::position blend with. Update overwrites
  // it with its own remainder. Out-of-range values are clamped; NaN is
  // ignored.
  void SetInterpolationAlpha(float alpha);

  // Returns the pose of `id` interpolated between its last two steps by the
  // accumulator remainder, for smooth rendering. Static bodies return their
  // fixed pose. An unknown handle returns an identity pose.
  Pose InterpolatedPose(BodyId id) const;

  glm::vec3 LinearVelocity(BodyId id) const;
  bool IsDynamic(BodyId id) const;
  // Returns whether the body is awake. False for static, sleeping and
  // unknown bodies.
  bool IsActive(BodyId id) const;

  // Returns the pose, velocities and activity of `id`; a default BodyState
  // for an unknown handle.
  BodyState GetBodyState(BodyId id) const;

  // Puts `id` into `state` instantly (a rewind, not a simulated motion): its
  // interpolation restarts from the new pose. Velocities are ignored for
  // static bodies and putting a body to sleep zeroes them, like Jolt does.
  // Unknown or stale handles are ignored.
  void SetBodyState(BodyId id, const BodyState& state);

  // Switches a dynamic body to kinematic (moved only by MoveKinematic,
  // unaffected by forces and collisions) or back to dynamic. Ignored for
  // static bodies and unknown handles.
  void SetKinematic(BodyId id, bool kinematic);

  // Sets the velocities of the kinematic body `id` so that the next
  // `dt` seconds of simulation bring it to `target`. Ignored for bodies that
  // are not kinematic, unknown handles and non-positive `dt`.
  void MoveKinematic(BodyId id, const Pose& target, float dt);

  PhysicsStats Stats() const;

 private:
  // The character reaches the simulation behind the world through Impl.
  friend class Character;
  // A ragdoll registers its bodies and constraints through Impl.
  friend class Ragdoll;

  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_PHYSICS_WORLD_H_
