#include "physics/physics_world.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
#include <unordered_map>
#include <utility>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "physics/internal/cooked_shape_access.h"
#include "physics/internal/jolt_math.h"
#include "physics/internal/layers.h"
#include "physics/internal/physics_world_impl.h"
#include "physics/internal/shape_builder.h"
#include "physics/physics_runtime.h"

namespace blue::physics {
namespace {

// Simulation step and the accumulator limits that keep a slow frame from
// spiraling into ever more steps.
constexpr double kFixedStep = 1.0 / 60.0;
constexpr double kMaxFrameSeconds = 0.25;
constexpr int kMaxStepsPerUpdate = 4;
// Absorbs float rounding so e.g. 60 calls of 1/60 s yield exactly 60 steps.
constexpr double kStepEpsilon = 1.0e-6;
// Callers that step the world themselves rely on the published step being the
// one the world simulates with.
static_assert(static_cast<float>(kFixedStep) == kFixedStepSeconds);

// Capacity of the simulation.
constexpr JPH::uint kMaxBodies = 65536;
constexpr JPH::uint kNumBodyMutexes = 0;  // 0 selects Jolt's default.
constexpr JPH::uint kMaxBodyPairs = 65536;
constexpr JPH::uint kMaxContactConstraints = 10240;
constexpr size_t kTempAllocatorBytes = 16 * 1024 * 1024;

// Adding at least this many bodies since the last broad phase optimization
// triggers another one (the first step always optimizes pending bodies).
constexpr int kBulkAddThreshold = 64;

// Jolt's default linear damping (0.05/s) would slow every body like air drag
// and make forces and free fall deviate from F = m * a; bodies get none.
// Angular damping keeps Jolt's default so rolling objects eventually stop.
constexpr float kLinearDamping = 0.0f;

using internal::FromJolt;
using internal::ToJolt;
namespace layers = internal::layers;

// Returns the origin pose of the body (not its center of mass).
Pose ReadPose(const JPH::BodyInterface& bodies, JPH::BodyID id) {
  JPH::RVec3 position;
  JPH::Quat rotation;
  bodies.GetPositionAndRotation(id, position, rotation);
  return Pose{FromJolt(JPH::Vec3(position)), FromJolt(rotation)};
}

bool IsFiniteVec(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Returns whether `rotation` is finite and long enough to normalize.
bool IsValidRotation(const glm::quat& rotation) {
  const float length = glm::length(rotation);
  return std::isfinite(length) && length > 1.0e-6f;
}

// Returns the fraction of a fixed step that `accumulator` seconds make up,
// clamped to [0, 1].
float AlphaOf(double accumulator) {
  return static_cast<float>(std::clamp(accumulator / kFixedStep, 0.0, 1.0));
}

// Returns the number of job threads for `options`.
int WorkerThreads(const PhysicsWorldOptions& options) {
  if (options.worker_threads >= 0) {
    return options.worker_threads;
  }
  return std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
}

// Returns whether `desc` can be turned into a body, ignoring the shape.
bool IsValidScale(glm::vec3 scale) {
  return IsFiniteVec(scale) && std::abs(scale.x) > 1.0e-4f &&
         std::abs(scale.y) > 1.0e-4f && std::abs(scale.z) > 1.0e-4f;
}

}  // namespace

const char* ToString(PhysicsError error) {
  switch (error) {
    case PhysicsError::kInvalidShape:
      return "invalid shape";
    case PhysicsError::kDynamicTriangleMesh:
      return "dynamic triangle mesh";
    case PhysicsError::kTooManyBodies:
      return "too many bodies";
    case PhysicsError::kShapeBuildFailed:
      return "shape build failed";
  }
  return "unknown physics error";
}

PhysicsWorld::Impl::Impl(const PhysicsWorldOptions& options)
    : temp_allocator_(kTempAllocatorBytes),
      job_system_(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                  WorkerThreads(options)) {
  system_.Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs,
               kMaxContactConstraints, layer_mapping_, object_vs_broad_,
               object_pair_);
}

PhysicsWorld::Impl::~Impl() {
  RemoveAllRagdolls();
  if (!participants_.empty()) {
    core::log::Warn(
        "PhysicsWorld destroyed with {} fixed-step participant(s) still "
        "registered; destroy characters before their world",
        participants_.size());
  }
}

void PhysicsWorld::Impl::AddParticipant(
    internal::FixedStepParticipant* participant) {
  if (participant == nullptr ||
      std::ranges::find(participants_, participant) != participants_.end()) {
    return;
  }
  participants_.push_back(participant);
}

void PhysicsWorld::Impl::RemoveParticipant(
    internal::FixedStepParticipant* participant) {
  std::erase(participants_, participant);
}

void PhysicsWorld::Impl::set_interpolation_alpha(float alpha) {
  if (!std::isnan(alpha)) {
    interpolation_alpha_ = std::clamp(alpha, 0.0f, 1.0f);
  }
}

core::Result<JPH::ShapeRefC, PhysicsError> PhysicsWorld::Impl::GetShape(
    const BodyDesc& desc) {
  using Result = core::Result<JPH::ShapeRefC, PhysicsError>;
  std::string key;
  if (!desc.shape_key.empty()) {
    key = desc.shape_key + '#' +
          std::to_string(static_cast<int>(desc.body.shape));
    if (auto cached = shape_cache_.find(key); cached != shape_cache_.end()) {
      return internal::ApplyScale(cached->second, desc.scale);
    }
  }
  Result base = internal::BuildBaseShape(desc);
  if (!base.ok()) {
    return base;
  }
  if (!key.empty()) {
    shape_cache_.emplace(std::move(key), base.value());
  }
  return internal::ApplyScale(base.value(), desc.scale);
}

core::Result<BodyId, PhysicsError> PhysicsWorld::Impl::CreateBody(
    const BodyDesc& desc) {
  using Result = core::Result<BodyId, PhysicsError>;
  const bool dynamic = desc.body.motion == scene::MotionType::kDynamic;
  if (dynamic && desc.body.shape == scene::ShapeType::kTriangleMesh) {
    return Result::Error(PhysicsError::kDynamicTriangleMesh);
  }
  if (std::optional<std::string_view> problem =
          scene::ValidateRigidBody(desc.body);
      problem.has_value()) {
    core::log::Warn("Physics body rejected: {}", *problem);
    return Result::Error(PhysicsError::kInvalidShape);
  }
  if (!IsValidScale(desc.scale) || !IsFiniteVec(desc.pose.position)) {
    return Result::Error(PhysicsError::kInvalidShape);
  }

  core::Result<JPH::ShapeRefC, PhysicsError> shape = GetShape(desc);
  if (!shape.ok()) {
    return Result::Error(shape.error());
  }

  return AddBody(shape.value().GetPtr(),
                 ShapeBodyDesc{.motion = desc.body.motion,
                               .pose = desc.pose,
                               .scale = desc.scale,
                               .mass = desc.body.mass,
                               .friction = desc.body.friction,
                               .restitution = desc.body.restitution});
}

core::Result<BodyId, PhysicsError> PhysicsWorld::Impl::CreateBody(
    JPH::ShapeRefC shape, const ShapeBodyDesc& desc) {
  using Result = core::Result<BodyId, PhysicsError>;
  const bool dynamic = desc.motion == scene::MotionType::kDynamic;
  if (dynamic && shape->GetSubType() == JPH::EShapeSubType::Mesh) {
    return Result::Error(PhysicsError::kDynamicTriangleMesh);
  }
  const scene::RigidBody body{.motion = desc.motion,
                              .mass = desc.mass,
                              .friction = desc.friction,
                              .restitution = desc.restitution};
  if (scene::ValidateRigidBody(body).has_value() || !IsValidScale(desc.scale) ||
      !IsFiniteVec(desc.pose.position)) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  core::Result<JPH::ShapeRefC, PhysicsError> scaled =
      internal::ApplyScale(shape, desc.scale);
  if (!scaled.ok()) {
    return Result::Error(scaled.error());
  }
  return AddBody(scaled.value().GetPtr(), desc);
}

core::Result<BodyId, PhysicsError> PhysicsWorld::Impl::AddBody(
    const JPH::Shape* shape, const ShapeBodyDesc& desc) {
  using Result = core::Result<BodyId, PhysicsError>;
  const bool dynamic = desc.motion == scene::MotionType::kDynamic;
  const JPH::ObjectLayer moving_layer =
      desc.blocks_characters ? layers::kMoving : layers::kMovingNoCharacter;
  JPH::BodyCreationSettings settings(
      shape, ToJolt(desc.pose.position),
      ToJolt(glm::normalize(desc.pose.rotation)),
      dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
      dynamic ? moving_layer : layers::kNonMoving);
  settings.mFriction = desc.friction;
  settings.mRestitution = desc.restitution;
  settings.mLinearDamping = kLinearDamping;
  if (dynamic) {
    settings.mOverrideMassProperties =
        JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = desc.mass;
    if (IsFiniteVec(desc.linear_velocity) &&
        IsFiniteVec(desc.angular_velocity)) {
      settings.mLinearVelocity = ToJolt(desc.linear_velocity);
      settings.mAngularVelocity = ToJolt(desc.angular_velocity);
    }
  }

  JPH::BodyInterface& bodies = system_.GetBodyInterface();
  const JPH::BodyID id = bodies.CreateAndAddBody(
      settings,
      dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  if (id.IsInvalid()) {
    return Result::Error(PhysicsError::kTooManyBodies);
  }
  ++bodies_added_since_optimize_;
  if (dynamic) {
    const Pose pose = ReadPose(bodies, id);
    tracked_[id.GetIndexAndSequenceNumber()] = TrackedPose{pose, pose};
  }
  return Result::Ok(FromJolt(id));
}

void PhysicsWorld::Impl::RemoveBody(BodyId id) {
  const JPH::BodyID jolt_id = ToJolt(id);
  if (ragdoll_bodies_.contains(jolt_id.GetIndexAndSequenceNumber())) {
    return;
  }
  JPH::BodyInterface& bodies = system_.GetBodyInterface();
  if (!bodies.IsAdded(jolt_id)) {
    return;
  }
  bodies.RemoveBody(jolt_id);
  bodies.DestroyBody(jolt_id);
  tracked_.erase(jolt_id.GetIndexAndSequenceNumber());
}

void PhysicsWorld::Impl::RemoveAllBodies() {
  RemoveAllRagdolls();
  JPH::BodyIDVector ids;
  system_.GetBodies(ids);
  JPH::BodyInterface& bodies = system_.GetBodyInterface();
  for (const JPH::BodyID& id : ids) {
    if (bodies.IsAdded(id)) {
      bodies.RemoveBody(id);
    }
  }
  bodies.DestroyBodies(ids.data(), static_cast<int>(ids.size()));
  tracked_.clear();
  bodies_added_since_optimize_ = 0;
}

core::Result<uint32_t, PhysicsError> PhysicsWorld::Impl::AddRagdoll(
    const JPH::RagdollSettings& settings, std::span<const Pose> poses,
    glm::vec3 linear_velocity, std::vector<BodyId>& bodies) {
  using Result = core::Result<uint32_t, PhysicsError>;
  if (poses.size() != settings.mParts.size() || !IsFiniteVec(linear_velocity)) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  for (const Pose& pose : poses) {
    if (!IsFiniteVec(pose.position) || !IsValidRotation(pose.rotation)) {
      return Result::Error(PhysicsError::kInvalidShape);
    }
  }

  const uint32_t key = next_ragdoll_key_;
  JPH::Ref<JPH::Ragdoll> ragdoll(settings.CreateRagdoll(key, key, &system_));
  if (ragdoll == nullptr) {
    return Result::Error(PhysicsError::kTooManyBodies);
  }
  ++next_ragdoll_key_;

  // The constraints were built with the parts at their rest poses; moving
  // the parts afterwards only changes where the ragdoll starts. Poses are set
  // before the bodies enter the system, velocities after (a velocity set
  // earlier would be lost).
  JPH::BodyInterface& body_interface = system_.GetBodyInterface();
  for (size_t i = 0; i < poses.size(); ++i) {
    body_interface.SetPositionAndRotation(
        ragdoll->GetBodyID(static_cast<int>(i)),
        JPH::RVec3(ToJolt(poses[i].position)),
        ToJolt(glm::normalize(poses[i].rotation)),
        JPH::EActivation::DontActivate);
  }
  ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
  ragdoll->SetLinearVelocity(ToJolt(linear_velocity));

  bodies.clear();
  for (const JPH::BodyID& id : ragdoll->GetBodyIDs()) {
    const Pose pose = ReadPose(body_interface, id);
    tracked_[id.GetIndexAndSequenceNumber()] = TrackedPose{pose, pose};
    ragdoll_bodies_.insert(id.GetIndexAndSequenceNumber());
    bodies.push_back(FromJolt(id));
  }
  ragdolls_.emplace(key, std::move(ragdoll));
  return Result::Ok(key);
}

void PhysicsWorld::Impl::RemoveRagdoll(uint32_t key) {
  const auto found = ragdolls_.find(key);
  if (found == ragdolls_.end()) {
    return;
  }
  JPH::Ragdoll& ragdoll = *found->second;
  ragdoll.RemoveFromPhysicsSystem();
  for (const JPH::BodyID& id : ragdoll.GetBodyIDs()) {
    tracked_.erase(id.GetIndexAndSequenceNumber());
    ragdoll_bodies_.erase(id.GetIndexAndSequenceNumber());
  }
  // The last reference destroys the bodies.
  ragdolls_.erase(found);
}

void PhysicsWorld::Impl::RemoveAllRagdolls() {
  while (!ragdolls_.empty()) {
    RemoveRagdoll(ragdolls_.begin()->first);
  }
}

bool PhysicsWorld::Impl::HasRagdoll(uint32_t key) const {
  return ragdolls_.contains(key);
}

bool PhysicsWorld::Impl::IsRagdollActive(uint32_t key) const {
  const auto found = ragdolls_.find(key);
  return found != ragdolls_.end() && found->second->IsActive();
}

void PhysicsWorld::Impl::Step() {
  for (auto& [key, tracked] : tracked_) {
    tracked.previous = tracked.current;
  }
  for (internal::FixedStepParticipant* participant : participants_) {
    participant->PreStep(static_cast<float>(kFixedStep));
  }
  const JPH::EPhysicsUpdateError error = system_.Update(
      static_cast<float>(kFixedStep), 1, &temp_allocator_, &job_system_);
  if (error != JPH::EPhysicsUpdateError::None) {
    core::log::Warn("Physics step reported error flags {}",
                    static_cast<int>(error));
  }
  const JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
  for (auto& [key, tracked] : tracked_) {
    tracked.current = ReadPose(bodies, JPH::BodyID(key));
  }
}

void PhysicsWorld::Impl::Update(float frame_seconds) {
  if (!(frame_seconds > 0.0f)) {  // Also rejects NaN.
    return;
  }
  accumulator_ +=
      std::min(static_cast<double>(frame_seconds), kMaxFrameSeconds);
  const int due = static_cast<int>(
      std::floor((accumulator_ + kStepEpsilon * kFixedStep) / kFixedStep));
  if (due <= 0) {
    interpolation_alpha_ = AlphaOf(accumulator_);
    return;
  }
  const int steps = std::min(due, kMaxStepsPerUpdate);
  accumulator_ = std::max(0.0, accumulator_ - steps * kFixedStep);
  if (due > kMaxStepsPerUpdate) {
    // Drop the backlog instead of spiraling; keep only the sub-step remainder.
    accumulator_ = std::fmod(accumulator_, kFixedStep);
  }
  interpolation_alpha_ = AlphaOf(accumulator_);
  RunSteps(steps);
}

void PhysicsWorld::Impl::RunSteps(int count) {
  if (count <= 0) {
    return;
  }
  if (bodies_added_since_optimize_ > 0 &&
      (!has_stepped_ || bodies_added_since_optimize_ >= kBulkAddThreshold)) {
    system_.OptimizeBroadPhase();
    bodies_added_since_optimize_ = 0;
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < count; ++i) {
    Step();
  }
  has_stepped_ = true;
  const std::chrono::duration<float, std::milli> elapsed =
      std::chrono::steady_clock::now() - start;
  last_step_ms_ = elapsed.count();
}

Pose PhysicsWorld::Impl::InterpolatedPose(BodyId id) const {
  if (auto tracked = tracked_.find(static_cast<uint32_t>(id));
      tracked != tracked_.end()) {
    const float alpha = interpolation_alpha();
    const Pose& from = tracked->second.previous;
    const Pose& to = tracked->second.current;
    return Pose{glm::mix(from.position, to.position, alpha),
                glm::slerp(from.rotation, to.rotation, alpha)};
  }
  const JPH::BodyID jolt_id = ToJolt(id);
  const JPH::BodyInterface& bodies = system_.GetBodyInterface();
  if (!bodies.IsAdded(jolt_id)) {
    return Pose{};
  }
  return ReadPose(bodies, jolt_id);
}

BodyState PhysicsWorld::Impl::GetBodyState(BodyId id) const {
  const JPH::BodyID jolt_id = ToJolt(id);
  const JPH::BodyInterface& bodies = system_.GetBodyInterface();
  if (!bodies.IsAdded(jolt_id)) {
    return BodyState{};
  }
  JPH::Vec3 linear;
  JPH::Vec3 angular;
  bodies.GetLinearAndAngularVelocity(jolt_id, linear, angular);
  return BodyState{.pose = ReadPose(bodies, jolt_id),
                   .linear_velocity = FromJolt(linear),
                   .angular_velocity = FromJolt(angular),
                   .active = bodies.IsActive(jolt_id)};
}

void PhysicsWorld::Impl::SetBodyState(BodyId id, const BodyState& state) {
  const JPH::BodyID jolt_id = ToJolt(id);
  JPH::BodyInterface& bodies = system_.GetBodyInterface();
  if (!bodies.IsAdded(jolt_id) || !IsFiniteVec(state.pose.position) ||
      !IsValidRotation(state.pose.rotation) ||
      !IsFiniteVec(state.linear_velocity) ||
      !IsFiniteVec(state.angular_velocity)) {
    return;
  }
  bodies.SetPositionAndRotation(jolt_id,
                                JPH::RVec3(ToJolt(state.pose.position)),
                                ToJolt(glm::normalize(state.pose.rotation)),
                                JPH::EActivation::DontActivate);
  if (bodies.GetMotionType(jolt_id) != JPH::EMotionType::Static) {
    bodies.SetLinearAndAngularVelocity(jolt_id, ToJolt(state.linear_velocity),
                                       ToJolt(state.angular_velocity));
    if (state.active) {
      bodies.ActivateBody(jolt_id);
    } else {
      bodies.DeactivateBody(jolt_id);
    }
  }
  if (auto tracked = tracked_.find(static_cast<uint32_t>(id));
      tracked != tracked_.end()) {
    const Pose pose = ReadPose(bodies, jolt_id);
    tracked->second = TrackedPose{pose, pose};
  }
}

PhysicsWorld::PhysicsWorld(const PhysicsRuntime& runtime)
    : PhysicsWorld(runtime, PhysicsWorldOptions{}) {}

PhysicsWorld::PhysicsWorld(const PhysicsRuntime& /*runtime*/,
                           const PhysicsWorldOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

PhysicsWorld::~PhysicsWorld() = default;

core::Result<BodyId, PhysicsError> PhysicsWorld::CreateBody(
    const BodyDesc& desc) {
  return impl_->CreateBody(desc);
}

core::Result<BodyId, PhysicsError> PhysicsWorld::CreateBody(
    const CookedShape& shape, const ShapeBodyDesc& desc) {
  return impl_->CreateBody(internal::CookedShapeAccess::Shape(shape), desc);
}

void PhysicsWorld::RemoveBody(BodyId id) { impl_->RemoveBody(id); }

void PhysicsWorld::RemoveAllBodies() { impl_->RemoveAllBodies(); }

void PhysicsWorld::SetGravity(glm::vec3 gravity) {
  impl_->system().SetGravity(ToJolt(gravity));
}

void PhysicsWorld::AddForce(BodyId id, glm::vec3 force) {
  impl_->system().GetBodyInterface().AddForce(ToJolt(id), ToJolt(force));
}

void PhysicsWorld::AddImpulse(BodyId id, glm::vec3 impulse) {
  impl_->system().GetBodyInterface().AddImpulse(ToJolt(id), ToJolt(impulse));
}

void PhysicsWorld::AddImpulseAtPoint(BodyId id, glm::vec3 impulse,
                                     glm::vec3 world_point) {
  impl_->system().GetBodyInterface().AddImpulse(
      ToJolt(id), ToJolt(impulse),
      JPH::RVec3(world_point.x, world_point.y, world_point.z));
}

std::optional<RayHit> PhysicsWorld::CastRay(glm::vec3 origin,
                                            glm::vec3 direction,
                                            float max_distance) const {
  return CastRay(origin, direction, max_distance, RayFilter::kGameplay);
}

std::optional<RayHit> PhysicsWorld::CastRay(glm::vec3 origin,
                                            glm::vec3 direction,
                                            float max_distance,
                                            RayFilter filter) const {
  const float length = glm::length(direction);
  if (!(max_distance > 0.0f) || !(length > 1.0e-6f) || !IsFiniteVec(origin)) {
    return std::nullopt;
  }
  const glm::vec3 unit_direction = direction / length;
  const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z),
                          ToJolt(unit_direction * max_distance));
  JPH::RayCastResult hit;
  const JPH::PhysicsSystem& system = impl_->system();
  const internal::GameplayObjectFilter gameplay_filter;
  const bool hit_something =
      filter == RayFilter::kGameplay
          ? system.GetNarrowPhaseQuery().CastRay(
                ray, hit, JPH::BroadPhaseLayerFilter(), gameplay_filter)
          : system.GetNarrowPhaseQuery().CastRay(ray, hit);
  if (!hit_something) {
    return std::nullopt;
  }
  const glm::vec3 point =
      origin + unit_direction * (max_distance * hit.mFraction);
  glm::vec3 normal(0.0f, 1.0f, 0.0f);
  const JPH::BodyLockRead lock(system.GetBodyLockInterface(), hit.mBodyID);
  if (lock.Succeeded()) {
    normal = FromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(
        hit.mSubShapeID2, JPH::RVec3(point.x, point.y, point.z)));
  }
  return RayHit{FromJolt(hit.mBodyID), point, normal, hit.mFraction};
}

void PhysicsWorld::Update(float frame_seconds) { impl_->Update(frame_seconds); }

void PhysicsWorld::StepOnce() { impl_->RunSteps(1); }

void PhysicsWorld::SetInterpolationAlpha(float alpha) {
  impl_->set_interpolation_alpha(alpha);
}

Pose PhysicsWorld::InterpolatedPose(BodyId id) const {
  return impl_->InterpolatedPose(id);
}

glm::vec3 PhysicsWorld::LinearVelocity(BodyId id) const {
  const JPH::BodyID jolt_id = ToJolt(id);
  const JPH::BodyInterface& bodies = impl_->system().GetBodyInterface();
  return bodies.IsAdded(jolt_id) ? FromJolt(bodies.GetLinearVelocity(jolt_id))
                                 : glm::vec3(0.0f);
}

bool PhysicsWorld::IsDynamic(BodyId id) const {
  const JPH::BodyID jolt_id = ToJolt(id);
  const JPH::BodyInterface& bodies = impl_->system().GetBodyInterface();
  return bodies.IsAdded(jolt_id) &&
         bodies.GetMotionType(jolt_id) == JPH::EMotionType::Dynamic;
}

bool PhysicsWorld::IsActive(BodyId id) const {
  const JPH::BodyID jolt_id = ToJolt(id);
  const JPH::BodyInterface& bodies = impl_->system().GetBodyInterface();
  return bodies.IsAdded(jolt_id) && bodies.IsActive(jolt_id);
}

BodyState PhysicsWorld::GetBodyState(BodyId id) const {
  return impl_->GetBodyState(id);
}

void PhysicsWorld::SetBodyState(BodyId id, const BodyState& state) {
  impl_->SetBodyState(id, state);
}

void PhysicsWorld::SetKinematic(BodyId id, bool kinematic) {
  const JPH::BodyID jolt_id = ToJolt(id);
  JPH::BodyInterface& bodies = impl_->system().GetBodyInterface();
  if (!bodies.IsAdded(jolt_id) ||
      bodies.GetMotionType(jolt_id) == JPH::EMotionType::Static) {
    return;
  }
  bodies.SetMotionType(
      jolt_id,
      kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic,
      JPH::EActivation::Activate);
}

void PhysicsWorld::MoveKinematic(BodyId id, const Pose& target, float dt) {
  const JPH::BodyID jolt_id = ToJolt(id);
  JPH::BodyInterface& bodies = impl_->system().GetBodyInterface();
  if (!(dt > 0.0f) || !std::isfinite(dt) || !IsFiniteVec(target.position) ||
      !IsValidRotation(target.rotation) || !bodies.IsAdded(jolt_id) ||
      bodies.GetMotionType(jolt_id) != JPH::EMotionType::Kinematic) {
    return;
  }
  bodies.MoveKinematic(jolt_id, JPH::RVec3(ToJolt(target.position)),
                       ToJolt(glm::normalize(target.rotation)), dt);
}

PhysicsStats PhysicsWorld::Stats() const {
  const JPH::PhysicsSystem& system = impl_->system();
  return PhysicsStats{
      static_cast<int>(system.GetNumBodies()),
      static_cast<int>(system.GetNumActiveBodies(JPH::EBodyType::RigidBody)),
      impl_->last_step_ms()};
}

}  // namespace blue::physics
