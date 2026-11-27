#include "physics/ragdoll.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
// clang-format on

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include "core/result.h"
#include "physics/internal/cooked_shape_access.h"
#include "physics/internal/jolt_math.h"
#include "physics/internal/layers.h"
#include "physics/internal/physics_world_impl.h"
#include "physics/internal/ragdoll_data.h"

namespace blue::physics {
namespace {

using internal::ToJolt;
namespace layers = internal::layers;

constexpr size_t kMaxParts = 64;
// A frame direction shorter than this (or a reference this close to parallel
// to its primary) has no usable direction.
constexpr float kMinAxisLength = 1.0e-3f;

bool IsFinite(float value) { return std::isfinite(value); }

bool IsFinite(glm::vec3 v) {
  return IsFinite(v.x) && IsFinite(v.y) && IsFinite(v.z);
}

bool InRange(float value, float min, float max) {
  return IsFinite(value) && value >= min && value <= max;
}

// A joint frame with unit, perpendicular directions.
struct Frame {
  glm::vec3 primary{0.0f, 1.0f, 0.0f};
  glm::vec3 reference{1.0f, 0.0f, 0.0f};
};

// Normalises `in.primary` and makes `in.reference` unit length and
// perpendicular to it. Returns false for non-finite, zero or (nearly)
// parallel directions.
bool Orthonormalize(const RagdollJointFrame& in, Frame& out) {
  if (!IsFinite(in.primary) || !IsFinite(in.reference) ||
      glm::length(in.primary) < kMinAxisLength ||
      glm::length(in.reference) < kMinAxisLength) {
    return false;
  }
  out.primary = glm::normalize(in.primary);
  const glm::vec3 reference =
      glm::normalize(in.reference) -
      out.primary * glm::dot(glm::normalize(in.reference), out.primary);
  if (glm::length(reference) < kMinAxisLength) {
    return false;
  }
  out.reference = glm::normalize(reference);
  return true;
}

// Returns the constraint joining a part to its parent, or null when the joint
// description is invalid.
JPH::Ref<JPH::TwoBodyConstraintSettings> MakeConstraint(
    const RagdollJointDesc& joint) {
  Frame parent;
  Frame child;
  if (!Orthonormalize(joint.parent, parent) ||
      !Orthonormalize(joint.child, child) || !IsFinite(joint.anchor) ||
      !IsFinite(joint.friction_torque) || joint.friction_torque < 0.0f) {
    return nullptr;
  }
  const JPH::RVec3 anchor(ToJolt(joint.anchor));
  if (joint.kind == RagdollJointKind::kHinge) {
    // Jolt only accepts min == max together with a spring; a hinge that
    // cannot move is not a hinge.
    if (!InRange(joint.hinge_min_degrees, -180.0f, 0.0f) ||
        !InRange(joint.hinge_max_degrees, 0.0f, 180.0f) ||
        !(joint.hinge_max_degrees > joint.hinge_min_degrees)) {
      return nullptr;
    }
    JPH::Ref<JPH::HingeConstraintSettings> hinge =
        new JPH::HingeConstraintSettings;
    hinge->mSpace = JPH::EConstraintSpace::WorldSpace;
    hinge->mPoint1 = anchor;
    hinge->mHingeAxis1 = ToJolt(parent.primary);
    hinge->mNormalAxis1 = ToJolt(parent.reference);
    hinge->mPoint2 = anchor;
    hinge->mHingeAxis2 = ToJolt(child.primary);
    hinge->mNormalAxis2 = ToJolt(child.reference);
    hinge->mLimitsMin = glm::radians(joint.hinge_min_degrees);
    hinge->mLimitsMax = glm::radians(joint.hinge_max_degrees);
    hinge->mMaxFrictionTorque = joint.friction_torque;
    return JPH::Ref<JPH::TwoBodyConstraintSettings>(hinge.GetPtr());
  }

  if (!InRange(joint.cone_reference_degrees, 0.0f, 180.0f) ||
      !InRange(joint.cone_side_degrees, 0.0f, 180.0f) ||
      !InRange(joint.twist_min_degrees, -180.0f, 0.0f) ||
      !InRange(joint.twist_max_degrees, 0.0f, 180.0f)) {
    return nullptr;
  }
  JPH::Ref<JPH::SwingTwistConstraintSettings> swing_twist =
      new JPH::SwingTwistConstraintSettings;
  swing_twist->mSpace = JPH::EConstraintSpace::WorldSpace;
  swing_twist->mPosition1 = anchor;
  swing_twist->mTwistAxis1 = ToJolt(parent.primary);
  swing_twist->mPlaneAxis1 = ToJolt(parent.reference);
  swing_twist->mPosition2 = anchor;
  swing_twist->mTwistAxis2 = ToJolt(child.primary);
  swing_twist->mPlaneAxis2 = ToJolt(child.reference);
  swing_twist->mSwingType = JPH::ESwingType::Cone;
  // Jolt's "plane" half cone angle tilts the twist axis towards +-plane axis
  // (our reference); its "normal" one towards +-(plane x twist), which is
  // cross(reference, primary).
  swing_twist->mPlaneHalfConeAngle = glm::radians(joint.cone_reference_degrees);
  swing_twist->mNormalHalfConeAngle = glm::radians(joint.cone_side_degrees);
  swing_twist->mTwistMinAngle = glm::radians(joint.twist_min_degrees);
  swing_twist->mTwistMaxAngle = glm::radians(joint.twist_max_degrees);
  swing_twist->mMaxFrictionTorque = joint.friction_torque;
  return JPH::Ref<JPH::TwoBodyConstraintSettings>(swing_twist.GetPtr());
}

bool IsValidDesc(const RagdollDesc& desc) {
  const size_t count = desc.parts.size();
  if (count < 1 || count > kMaxParts) {
    return false;
  }
  if (!InRange(desc.rest_overlap_margin, 0.0f, 1.0e6f) ||
      !InRange(desc.friction, 0.0f, 1.0e6f) ||
      !InRange(desc.restitution, 0.0f, 1.0f) ||
      !InRange(desc.linear_damping, 0.0f, 1.0e6f) ||
      !InRange(desc.angular_damping, 0.0f, 1.0e6f) ||
      !(desc.max_linear_velocity > 0.0f) ||
      !IsFinite(desc.max_linear_velocity) ||
      !(desc.max_angular_velocity > 0.0f) ||
      !IsFinite(desc.max_angular_velocity)) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    const RagdollPartDesc& part = desc.parts[i];
    const bool parent_ok =
        i == 0 ? part.parent == -1
               : part.parent >= 0 && static_cast<size_t>(part.parent) < i;
    const float rotation_length = glm::length(part.rest_pose.rotation);
    if (!parent_ok || !IsFinite(part.rest_pose.position) ||
        !(rotation_length > 1.0e-6f) || !IsFinite(rotation_length) ||
        !(part.mass > 0.0f) || !IsFinite(part.mass)) {
      return false;
    }
  }
  const int part_count = static_cast<int>(count);
  for (const auto& [a, b] : desc.no_collide) {
    if (a < 0 || b < 0 || a >= part_count || b >= part_count || a == b) {
      return false;
    }
  }
  return true;
}

}  // namespace

core::Result<RagdollTemplate, PhysicsError> RagdollTemplate::Create(
    const RagdollDesc& desc) {
  using Result = core::Result<RagdollTemplate, PhysicsError>;
  if (!IsValidDesc(desc)) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  const size_t count = desc.parts.size();
  auto data = std::make_shared<Data>();
  data->parents.reserve(count);
  data->rest_poses.reserve(count);
  data->shapes.reserve(count);

  JPH::Ref<JPH::Skeleton> skeleton = new JPH::Skeleton;
  JPH::Ref<JPH::RagdollSettings> settings = new JPH::RagdollSettings;
  settings->mParts.resize(count);
  std::vector<JPH::Mat44> rest_matrices;
  rest_matrices.reserve(count);

  for (size_t i = 0; i < count; ++i) {
    const RagdollPartDesc& part = desc.parts[i];
    if (part.shape.kind == CookedShapeKind::kTriangleMesh) {
      return Result::Error(PhysicsError::kInvalidShape);  // Static only.
    }
    core::Result<std::vector<std::byte>, PhysicsError> cooked =
        CookShape(part.shape);
    if (!cooked.ok()) {
      return Result::Error(cooked.error());
    }
    core::Result<CookedShape, PhysicsError> shape =
        CookedShape::Restore(cooked.value());
    if (!shape.ok()) {
      return Result::Error(shape.error());
    }

    const Pose rest{part.rest_pose.position,
                    glm::normalize(part.rest_pose.rotation)};
    skeleton->AddJoint("part" + std::to_string(i), part.parent);

    JPH::RagdollSettings::Part& jolt_part = settings->mParts[i];
    jolt_part.SetShape(internal::CookedShapeAccess::Shape(shape.value()));
    jolt_part.mPosition = JPH::RVec3(ToJolt(rest.position));
    jolt_part.mRotation = ToJolt(rest.rotation);
    jolt_part.mMotionType = JPH::EMotionType::Dynamic;
    jolt_part.mObjectLayer = layers::kCosmetic;
    jolt_part.mOverrideMassProperties =
        JPH::EOverrideMassProperties::CalculateInertia;
    jolt_part.mMassPropertiesOverride.mMass = part.mass;
    jolt_part.mFriction = desc.friction;
    jolt_part.mRestitution = desc.restitution;
    jolt_part.mLinearDamping = desc.linear_damping;
    jolt_part.mAngularDamping = desc.angular_damping;
    jolt_part.mMaxLinearVelocity = desc.max_linear_velocity;
    jolt_part.mMaxAngularVelocity = desc.max_angular_velocity;
    if (i > 0) {
      jolt_part.mToParent = MakeConstraint(part.joint);
      if (jolt_part.mToParent == nullptr) {
        return Result::Error(PhysicsError::kInvalidShape);
      }
    }

    rest_matrices.push_back(JPH::Mat44::sRotationTranslation(
        ToJolt(rest.rotation), ToJolt(rest.position)));
    data->parents.push_back(part.parent);
    data->rest_poses.push_back(rest);
    data->shapes.push_back(std::move(shape.value()));
  }
  settings->mSkeleton = skeleton;

  // Masses first (Stabilize rewrites them), then the collision table: parent
  // and child never collide, nor do parts that overlap at rest, nor the
  // explicit pairs.
  if (desc.stabilize && !settings->Stabilize()) {
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  settings->DisableParentChildCollisions(rest_matrices.data(),
                                         desc.rest_overlap_margin);
  if (!desc.no_collide.empty()) {
    // DisableParentChildCollisions just built this table and every part
    // shares it; nothing else refers to it yet, so extending it is safe.
    auto* table = const_cast<JPH::GroupFilterTable*>(
        static_cast<const JPH::GroupFilterTable*>(
            settings->mParts[0].mCollisionGroup.GetGroupFilter()));
    for (const auto& [a, b] : desc.no_collide) {
      table->DisableCollision(static_cast<JPH::CollisionGroup::SubGroupID>(a),
                              static_cast<JPH::CollisionGroup::SubGroupID>(b));
    }
  }
  settings->CalculateBodyIndexToConstraintIndex();
  settings->CalculateConstraintIndexToBodyIdxPair();
  settings->CalculateConstraintPriorities();
  data->settings = settings;

  RagdollTemplate tmpl;
  tmpl.data_ = std::move(data);
  return Result::Ok(std::move(tmpl));
}

int RagdollTemplate::part_count() const {
  return data_ ? static_cast<int>(data_->parents.size()) : 0;
}

int RagdollTemplate::parent(int part) const {
  if (!data_ || part < 0 || part >= part_count()) {
    return -1;
  }
  return data_->parents[static_cast<size_t>(part)];
}

Pose RagdollTemplate::rest_pose(int part) const {
  if (!data_ || part < 0 || part >= part_count()) {
    return Pose{};
  }
  return data_->rest_poses[static_cast<size_t>(part)];
}

const CookedShape& RagdollTemplate::shape(int part) const {
  return data_->shapes[static_cast<size_t>(part)];
}

core::Result<Ragdoll, PhysicsError> Ragdoll::Create(
    PhysicsWorld& world, const RagdollTemplate& tmpl,
    const RagdollSpawnDesc& spawn) {
  using Result = core::Result<Ragdoll, PhysicsError>;
  if (!tmpl.data_) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  Ragdoll ragdoll;
  core::Result<uint32_t, PhysicsError> key =
      world.impl_->AddRagdoll(*tmpl.data_->settings, spawn.poses,
                              spawn.linear_velocity, ragdoll.bodies_);
  if (!key.ok()) {
    return Result::Error(key.error());
  }
  ragdoll.world_ = &world;
  ragdoll.key_ = key.value();
  return Result::Ok(std::move(ragdoll));
}

Ragdoll::Ragdoll(Ragdoll&& other) noexcept
    : world_(std::exchange(other.world_, nullptr)),
      key_(std::exchange(other.key_, 0)),
      bodies_(std::move(other.bodies_)) {
  other.bodies_.clear();
}

Ragdoll& Ragdoll::operator=(Ragdoll&& other) noexcept {
  if (this != &other) {
    if (world_ != nullptr) {
      world_->impl_->RemoveRagdoll(key_);
    }
    world_ = std::exchange(other.world_, nullptr);
    key_ = std::exchange(other.key_, 0);
    bodies_ = std::move(other.bodies_);
    other.bodies_.clear();
  }
  return *this;
}

Ragdoll::~Ragdoll() {
  if (world_ != nullptr) {
    world_->impl_->RemoveRagdoll(key_);
  }
}

bool Ragdoll::valid() const {
  return world_ != nullptr && world_->impl_->HasRagdoll(key_);
}

std::span<const BodyId> Ragdoll::bodies() const {
  return valid() ? std::span<const BodyId>(bodies_) : std::span<const BodyId>();
}

bool Ragdoll::IsActive() const {
  return world_ != nullptr && world_->impl_->IsRagdollActive(key_);
}

}  // namespace blue::physics
