#ifndef BLUE_PHYSICS_INTERNAL_LAYERS_H_
#define BLUE_PHYSICS_INTERNAL_LAYERS_H_

// Internal to the physics library: this header exposes Jolt types, so it lives
// in internal/ and must not be included from the public physics headers.

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
// clang-format on

namespace blue::physics::internal {

// Two object layers: static geometry and everything that moves. Something
// collides only if at least one side is kMoving.
namespace layers {
inline constexpr JPH::ObjectLayer kNonMoving = 0;
inline constexpr JPH::ObjectLayer kMoving = 1;
inline constexpr JPH::uint kCount = 2;
}  // namespace layers

// One broad phase layer per object layer.
namespace broad_layers {
inline constexpr JPH::BroadPhaseLayer kNonMoving(0);
inline constexpr JPH::BroadPhaseLayer kMoving(1);
}  // namespace broad_layers

}  // namespace blue::physics::internal

#endif  // BLUE_PHYSICS_INTERNAL_LAYERS_H_
