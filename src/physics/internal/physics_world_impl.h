#ifndef BLUE_PHYSICS_INTERNAL_PHYSICS_WORLD_IMPL_H_
#define BLUE_PHYSICS_INTERNAL_PHYSICS_WORLD_IMPL_H_

// Internal to the physics library: this header exposes Jolt types, so it lives
// in internal/ and must not be included from the public physics headers. It
// defines PhysicsWorld::Impl so that other physics sources (the character)
// can reach the Jolt system behind a PhysicsWorld.

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
// clang-format on

#include "core/result.h"
#include "physics/internal/layers.h"
#include "physics/physics_world.h"

namespace blue::physics::internal {

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

// Something the world advances inside every fixed step, before the rigid
// bodies. Kinematic characters implement it. Not owned by the world; a
// participant must remove itself before it is destroyed.
class FixedStepParticipant {
 public:
  virtual ~FixedStepParticipant() = default;

  // Called once per fixed step with the step length in seconds.
  virtual void PreStep(float step_seconds) = 0;
};

}  // namespace blue::physics::internal

namespace blue::physics {

// Owns the Jolt objects. Members are declared in dependency order: the
// physics system refers to the layer objects, and everything is destroyed in
// reverse, so the system goes first.
class PhysicsWorld::Impl {
 public:
  Impl();
  Impl(const Impl&) = delete;
  Impl& operator=(const Impl&) = delete;
  ~Impl();

  core::Result<BodyId, PhysicsError> CreateBody(const BodyDesc& desc);
  void RemoveBody(BodyId id);
  void RemoveAllBodies();
  void Update(float frame_seconds);
  Pose InterpolatedPose(BodyId id) const;

  // Registers / unregisters a participant (not owned). Adding one twice or
  // removing an unknown one is ignored.
  void AddParticipant(internal::FixedStepParticipant* participant);
  void RemoveParticipant(internal::FixedStepParticipant* participant);

  // Fraction of the next fixed step already elapsed, in [0, 1], for blending
  // the previous and current step state.
  float interpolation_alpha() const;

  JPH::PhysicsSystem& system() { return system_; }
  const JPH::PhysicsSystem& system() const { return system_; }
  JPH::TempAllocator& temp_allocator() { return temp_allocator_; }
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

  internal::BroadPhaseLayerMapping layer_mapping_;
  internal::ObjectVsBroadPhaseFilter object_vs_broad_;
  internal::ObjectPairFilter object_pair_;
  JPH::TempAllocatorImpl temp_allocator_;
  JPH::JobSystemThreadPool job_system_;
  JPH::PhysicsSystem system_;
  // Unscaled shapes by shape_key + shape type, shared between bodies.
  std::unordered_map<std::string, JPH::ShapeRefC> shape_cache_;
  std::unordered_map<uint32_t, TrackedPose> tracked_;
  std::vector<internal::FixedStepParticipant*> participants_;

  double accumulator_ = 0.0;
  float last_step_ms_ = 0.0f;
  int bodies_added_since_optimize_ = 0;
  bool has_stepped_ = false;
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_INTERNAL_PHYSICS_WORLD_IMPL_H_
