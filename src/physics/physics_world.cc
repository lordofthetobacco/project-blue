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
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include "core/log.h"
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

// Two object layers: static geometry and everything that moves.
namespace layers {
constexpr JPH::ObjectLayer kNonMoving = 0;
constexpr JPH::ObjectLayer kMoving = 1;
constexpr JPH::uint kCount = 2;
}  // namespace layers

// One broad phase layer per object layer.
namespace broad_layers {
constexpr JPH::BroadPhaseLayer kNonMoving(0);
constexpr JPH::BroadPhaseLayer kMoving(1);
}  // namespace broad_layers

class BroadPhaseLayerMapping final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return layers::kCount; }

  JPH::BroadPhaseLayer GetBroadPhaseLayer(
      JPH::ObjectLayer layer) const override {
    return layer == layers::kNonMoving ? broad_layers::kNonMoving
                                       : broad_layers::kMoving;
  }

// Jolt only declares this hook when its profiler is compiled in.
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(
      JPH::BroadPhaseLayer layer) const override {
    return layer == broad_layers::kNonMoving ? "NON_MOVING" : "MOVING";
  }
#endif
};

class ObjectVsBroadPhaseFilter final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer,
                     JPH::BroadPhaseLayer broad_layer) const override {
    // Static geometry only collides with moving objects.
    return layer == layers::kMoving || broad_layer == broad_layers::kMoving;
  }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer1,
                     JPH::ObjectLayer layer2) const override {
    return layer1 == layers::kMoving || layer2 == layers::kMoving;
  }
};

JPH::Vec3 ToJolt(glm::vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
glm::vec3 FromJolt(JPH::Vec3 v) {
  return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

JPH::Quat ToJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
glm::quat FromJolt(JPH::Quat q) {
  return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

JPH::BodyID ToJolt(BodyId id) { return JPH::BodyID(static_cast<uint32_t>(id)); }
BodyId FromJolt(JPH::BodyID id) {
  return BodyId{id.GetIndexAndSequenceNumber()};
}

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

// Owns the Jolt objects. Members are declared in dependency order: the
// physics system refers to the layer objects, and everything is destroyed in
// reverse, so the system goes first.
class PhysicsWorld::Impl {
 public:
  Impl()
      : temp_allocator_(kTempAllocatorBytes),
        job_system_(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            std::max(
                1, static_cast<int>(std::thread::hardware_concurrency()) - 1)) {
    system_.Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs,
                 kMaxContactConstraints, layer_mapping_, object_vs_broad_,
                 object_pair_);
  }

  core::Result<BodyId, PhysicsError> CreateBody(const BodyDesc& desc);
  void RemoveBody(BodyId id);
  void RemoveAllBodies();
  void Update(float frame_seconds);
  Pose InterpolatedPose(BodyId id) const;

  JPH::PhysicsSystem& system() { return system_; }
  const JPH::PhysicsSystem& system() const { return system_; }
  float last_step_ms() const { return last_step_ms_; }

 private:
  // Previous and current step pose of a dynamic body.
  struct TrackedPose {
    Pose previous;
    Pose current;
  };

  // Returns the (cached) shape for `desc`, scale applied.
  core::Result<JPH::ShapeRefC, PhysicsError> GetShape(const BodyDesc& desc);
  // Runs one fixed step and refreshes the tracked poses.
  void Step();

  BroadPhaseLayerMapping layer_mapping_;
  ObjectVsBroadPhaseFilter object_vs_broad_;
  ObjectPairFilter object_pair_;
  JPH::TempAllocatorImpl temp_allocator_;
  JPH::JobSystemThreadPool job_system_;
  JPH::PhysicsSystem system_;
  // Unscaled shapes by shape_key + shape type, shared between bodies.
  std::unordered_map<std::string, JPH::ShapeRefC> shape_cache_;
  std::unordered_map<uint32_t, TrackedPose> tracked_;

  double accumulator_ = 0.0;
  float last_step_ms_ = 0.0f;
  int bodies_added_since_optimize_ = 0;
  bool has_stepped_ = false;
};

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

  JPH::BodyCreationSettings settings(
      shape.value().GetPtr(), ToJolt(desc.pose.position),
      ToJolt(glm::normalize(desc.pose.rotation)),
      dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
      dynamic ? layers::kMoving : layers::kNonMoving);
  settings.mFriction = desc.body.friction;
  settings.mRestitution = desc.body.restitution;
  settings.mLinearDamping = kLinearDamping;
  if (dynamic) {
    settings.mOverrideMassProperties =
        JPH::EOverrideMassProperties::CalculateInertia;
    settings.mMassPropertiesOverride.mMass = desc.body.mass;
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
  JPH::BodyInterface& bodies = system_.GetBodyInterface();
  if (!bodies.IsAdded(jolt_id)) {
    return;
  }
  bodies.RemoveBody(jolt_id);
  bodies.DestroyBody(jolt_id);
  tracked_.erase(jolt_id.GetIndexAndSequenceNumber());
}

void PhysicsWorld::Impl::RemoveAllBodies() {
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

void PhysicsWorld::Impl::Step() {
  for (auto& [key, tracked] : tracked_) {
    tracked.previous = tracked.current;
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
    return;
  }
  const int steps = std::min(due, kMaxStepsPerUpdate);
  accumulator_ = std::max(0.0, accumulator_ - steps * kFixedStep);
  if (due > kMaxStepsPerUpdate) {
    // Drop the backlog instead of spiraling; keep only the sub-step remainder.
    accumulator_ = std::fmod(accumulator_, kFixedStep);
  }

  if (bodies_added_since_optimize_ > 0 &&
      (!has_stepped_ || bodies_added_since_optimize_ >= kBulkAddThreshold)) {
    system_.OptimizeBroadPhase();
    bodies_added_since_optimize_ = 0;
  }

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < steps; ++i) {
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
    const float alpha =
        static_cast<float>(std::clamp(accumulator_ / kFixedStep, 0.0, 1.0));
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

PhysicsWorld::PhysicsWorld(const PhysicsRuntime& /*runtime*/)
    : impl_(std::make_unique<Impl>()) {}

PhysicsWorld::~PhysicsWorld() = default;

core::Result<BodyId, PhysicsError> PhysicsWorld::CreateBody(
    const BodyDesc& desc) {
  return impl_->CreateBody(desc);
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
  const float length = glm::length(direction);
  if (!(max_distance > 0.0f) || !(length > 1.0e-6f) || !IsFiniteVec(origin)) {
    return std::nullopt;
  }
  const glm::vec3 unit_direction = direction / length;
  const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z),
                          ToJolt(unit_direction * max_distance));
  JPH::RayCastResult hit;
  const JPH::PhysicsSystem& system = impl_->system();
  if (!system.GetNarrowPhaseQuery().CastRay(ray, hit)) {
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

PhysicsStats PhysicsWorld::Stats() const {
  const JPH::PhysicsSystem& system = impl_->system();
  return PhysicsStats{
      static_cast<int>(system.GetNumBodies()),
      static_cast<int>(system.GetNumActiveBodies(JPH::EBodyType::RigidBody)),
      impl_->last_step_ms()};
}

}  // namespace blue::physics
