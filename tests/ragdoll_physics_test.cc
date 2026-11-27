// Tests of physics::Ragdoll (src/physics/ragdoll.h): a chain falls and rests
// with finite poses, hinge and cone limits hold, a spawn outside the limits
// recovers without a kick, default ray casts and characters ignore the parts
// while RayFilter::kEverything hits them, RemoveAllBodies with a live ragdoll
// is safe, parts interpolate. Cases are added by the physics work package.

#include <doctest/doctest.h>

#include "physics/physics_runtime.h"
#include "physics/physics_world.h"
#include "physics/ragdoll.h"

namespace blue::physics {
namespace {}  // namespace
}  // namespace blue::physics
