#ifndef BLUE_PHYSICS_RAGDOLL_H_
#define BLUE_PHYSICS_RAGDOLL_H_

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "core/result.h"
#include "physics/physics_world.h"
#include "physics/shape_cooking.h"

// Ragdolls: articulated bodies made of rigid parts joined by limited
// constraints, for cosmetic use (killed characters). A RagdollDesc describes
// the parts and joints at rest; RagdollTemplate::Create validates and cooks it
// once into an immutable shared template; Ragdoll::Create spawns one instance
// into a PhysicsWorld from the world pose of every part.
//
// The parts live on the cosmetic object layer: gameplay ray casts and
// characters ignore them (see RayFilter), they take part in interpolation like
// any dynamic body. Ragdolls never influence the deterministic simulation.

namespace blue::physics {

enum class RagdollJointKind : uint8_t {
  kSwingTwist,  // Cone limit plus twist limit (shoulder, hip, spine, ...).
  kHinge,       // One axis (elbow, knee).
};

// A joint frame fixed in one part. Directions are in MODEL SPACE AT REST, unit
// length and perpendicular; RagdollTemplate::Create re-orthogonalises
// `reference` against `primary`.
//   kSwingTwist: primary = twist axis (child: bone direction; parent: cone
//                centre), reference = cone reference direction.
//   kHinge:      primary = hinge axis, reference = perpendicular (the bone
//                direction).
struct RagdollJointFrame {
  glm::vec3 primary{0.0f, 1.0f, 0.0f};
  glm::vec3 reference{1.0f, 0.0f, 0.0f};
};

// The joint between a part and its parent.
struct RagdollJointDesc {
  RagdollJointKind kind = RagdollJointKind::kSwingTwist;
  glm::vec3 anchor{0.0f};    // Model space at rest; shared by both parts.
  RagdollJointFrame parent;  // Fixed in the parent part.
  RagdollJointFrame child;   // Fixed in this part.
  // kSwingTwist: the child's primary axis may tilt from the parent's primary
  // axis by cone_reference_degrees towards +-parent.reference and by
  // cone_side_degrees towards +-cross(parent.reference, parent.primary); the
  // twist about it lies in [twist_min_degrees, twist_max_degrees].
  // Degrees; cones [0, 180], twist_min [-180, 0], twist_max [0, 180].
  float cone_reference_degrees = 0.0f;
  float cone_side_degrees = 0.0f;
  float twist_min_degrees = 0.0f;
  float twist_max_degrees = 0.0f;
  // kHinge: right-handed rotation of child.reference from parent.reference
  // about parent.primary; 0 when the frames coincide.
  float hinge_min_degrees = 0.0f;  // [-180, 0]
  float hinge_max_degrees = 0.0f;  // [0, 180]
  // Resistance of the joint to motion, N*m, >= 0.
  float friction_torque = 0.0f;
};

// One rigid part of a ragdoll.
struct RagdollPartDesc {
  int parent = -1;         // Index of an earlier part; -1 for part 0 only.
  ShapeCookDesc shape;     // In the part frame (box, sphere, capsule, hull).
  Pose rest_pose;          // The part frame in model space at rest.
  float mass = 1.0f;       // kg, > 0
  RagdollJointDesc joint;  // To the parent; ignored for part 0.
};

// Everything needed to build a RagdollTemplate. Spans inside the parts'
// ShapeCookDesc are borrowed for the duration of Create only.
struct RagdollDesc {
  std::vector<RagdollPartDesc> parts;  // Parents first; 1..64 parts.
  // Part pairs that must not collide besides parent/child pairs and pairs
  // that overlap at rest.
  std::vector<std::pair<int, int>> no_collide;
  float rest_overlap_margin = 0.02f;  // m
  float friction = 0.8f;
  float restitution = 0.0f;
  float linear_damping = 0.05f;        // 1/s
  float angular_damping = 0.4f;        // 1/s
  float max_linear_velocity = 30.0f;   // m/s
  float max_angular_velocity = 30.0f;  // rad/s
  bool stabilize = true;               // JPH::RagdollSettings::Stabilize()
};

// A validated, cooked RagdollDesc. Immutable and cheap to copy (copies share
// the data); usable by any number of ragdolls. Must be destroyed before the
// PhysicsRuntime, like CookedShape.
class RagdollTemplate {
 public:
  // Fails with kInvalidShape for a bad description (no parts, more than 64,
  // a parent that is not an earlier part, non-positive mass, limits outside
  // their ranges, non-finite values) and with the shape errors of CookShape.
  [[nodiscard]] static core::Result<RagdollTemplate, PhysicsError> Create(
      const RagdollDesc& desc);

  [[nodiscard]] int part_count() const;
  // Parent part of `part`, -1 for part 0 and out-of-range indices.
  [[nodiscard]] int parent(int part) const;
  // The part frame in model space at rest; identity for an out-of-range part.
  [[nodiscard]] Pose rest_pose(int part) const;
  // The part's shape, in the part frame. `part` must be in range.
  [[nodiscard]] const CookedShape& shape(int part) const;

 private:
  friend class Ragdoll;
  struct Data;  // src/physics/internal/ragdoll_data.h
  std::shared_ptr<const Data> data_;
};

// Where to spawn a ragdoll.
struct RagdollSpawnDesc {
  std::span<const Pose> poses;      // World pose of every part (part_count()).
  glm::vec3 linear_velocity{0.0f};  // m/s, every part.
};

// One ragdoll in a PhysicsWorld. Move-only; destroy it before its world.
// Destroying it removes its constraints and bodies.
class Ragdoll {
 public:
  // Spawns the ragdoll with the part poses of `spawn`. A pose outside a
  // joint's limits is legal: position correction turns it back inside over
  // the next steps, no velocity is injected. Fails with kInvalidShape when
  // the number of poses differs from the template's parts or a pose or the
  // velocity is not finite.
  [[nodiscard]] static core::Result<Ragdoll, PhysicsError> Create(
      PhysicsWorld& world, const RagdollTemplate& tmpl,
      const RagdollSpawnDesc& spawn);

  Ragdoll(Ragdoll&&) noexcept;
  Ragdoll& operator=(Ragdoll&&) noexcept;
  ~Ragdoll();

  // False after a move-from and after the world removed it
  // (PhysicsWorld::RemoveAllBodies).
  [[nodiscard]] bool valid() const;
  // The body of every part, in part order. Empty when not valid().
  [[nodiscard]] std::span<const BodyId> bodies() const;
  // Whether any part is awake.
  [[nodiscard]] bool IsActive() const;

 private:
  Ragdoll() = default;

  PhysicsWorld* world_ = nullptr;
  uint32_t key_ = 0;
  std::vector<BodyId> bodies_;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_RAGDOLL_H_
