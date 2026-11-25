#ifndef BLUE_NET_WORLD_OBJECT_H_
#define BLUE_NET_WORLD_OBJECT_H_

#include <cstdint>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include "net/bit_stream.h"

namespace blue::net {

// World objects (dynamic rigid bodies and dropped weapons), sent as absolute
// quantised state in every snapshot that has room for them, section 9.5 of
// docs/netcode/protocol.md.

// Which id space a world object's id belongs to (1 bit on the wire).
enum class ObjectKind : uint8_t {
  kBody = 0,           // Keyed by its scene instance_id.
  kDroppedWeapon = 1,  // Keyed by its dropped_id.
};

// State of one world object in simulation units.
struct WorldObject {
  ObjectKind kind = ObjectKind::kBody;
  uint32_t id = 0;
  // At rest: velocities are not sent and decode as zero.
  bool at_rest = false;
  glm::vec3 position = glm::vec3(0.0f);
  glm::quat rotation = glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f);
  glm::vec3 linear_velocity = glm::vec3(0.0f);   // m/s
  glm::vec3 angular_velocity = glm::vec3(0.0f);  // rad/s

  friend bool operator==(const WorldObject&, const WorldObject&) = default;
};

// Returns the object as a receiver will decode it (quantised and back).
WorldObject Quantized(const WorldObject& object);

// Writes the object (quantising it; never fails on values, they clamp).
void WriteWorldObject(const WorldObject& object, BitWriter& writer);
// Reads one object.
WorldObject ReadWorldObject(BitReader& reader);

}  // namespace blue::net

#endif  // BLUE_NET_WORLD_OBJECT_H_
