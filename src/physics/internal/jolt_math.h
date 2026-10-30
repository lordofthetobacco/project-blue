#ifndef BLUE_PHYSICS_INTERNAL_JOLT_MATH_H_
#define BLUE_PHYSICS_INTERNAL_JOLT_MATH_H_

// Internal to the physics library: this header exposes Jolt types, so it lives
// in internal/ and must not be included from the public physics headers.

#include <cstdint>

// Jolt.h must be the first Jolt header included.
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
// clang-format on

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "physics/physics_world.h"

namespace blue::physics::internal {

// Conversions between glm and Jolt value types. Quaternions are {w, x, y, z}
// in glm and {x, y, z, w} in Jolt.
inline JPH::Vec3 ToJolt(glm::vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
inline glm::vec3 FromJolt(JPH::Vec3 v) {
  return glm::vec3(v.GetX(), v.GetY(), v.GetZ());
}

inline JPH::Quat ToJolt(const glm::quat& q) {
  return JPH::Quat(q.x, q.y, q.z, q.w);
}
inline glm::quat FromJolt(JPH::Quat q) {
  return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

inline JPH::BodyID ToJolt(BodyId id) {
  return JPH::BodyID(static_cast<uint32_t>(id));
}
inline BodyId FromJolt(JPH::BodyID id) {
  return BodyId{id.GetIndexAndSequenceNumber()};
}

}  // namespace blue::physics::internal

#endif  // BLUE_PHYSICS_INTERNAL_JOLT_MATH_H_
