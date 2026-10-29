#ifndef BLUE_PHYSICS_PHYSICS_RUNTIME_H_
#define BLUE_PHYSICS_PHYSICS_RUNTIME_H_

namespace blue::physics {

// Process-wide Jolt setup: allocator, trace/assert hooks forwarding to
// core::log, the type factory and the registered Jolt types. Create exactly
// one before any PhysicsWorld and keep it alive for as long as any world
// exists. A second live instance is a programming error and terminates the
// process through log::Fatal. After destruction a new runtime may be created.
//
// Not copyable and not movable. Construct and destroy on one thread.
class PhysicsRuntime {
 public:
  PhysicsRuntime();
  PhysicsRuntime(const PhysicsRuntime&) = delete;
  PhysicsRuntime& operator=(const PhysicsRuntime&) = delete;
  ~PhysicsRuntime();
};

}  // namespace blue::physics

#endif  // BLUE_PHYSICS_PHYSICS_RUNTIME_H_
