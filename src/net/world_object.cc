#include "net/world_object.h"

#include <array>
#include <cstdint>

#include <glm/vec3.hpp>

#include "net/bit_stream.h"
#include "net/quantize.h"

namespace blue::net {
namespace {

constexpr int kLargestIndexBits = 2;

// An object in wire units.
struct ObjectQ {
  ObjectKind kind = ObjectKind::kBody;
  uint32_t id = 0;
  bool at_rest = false;
  std::array<int32_t, 3> position = {};
  QuatQ rotation;
  std::array<int32_t, 3> linear_velocity = {};
  std::array<int32_t, 3> angular_velocity = {};
};

ObjectQ ToWire(const WorldObject& object) {
  ObjectQ q;
  q.kind = object.kind;
  q.id = object.id;
  q.at_rest = object.at_rest;
  for (int axis = 0; axis < 3; ++axis) {
    q.position[axis] = QuantizePosition(object.position[axis]);
    if (!object.at_rest) {
      q.linear_velocity[axis] = QuantizeVelocity(object.linear_velocity[axis]);
      q.angular_velocity[axis] =
          QuantizeVelocity(object.angular_velocity[axis]);
    }
  }
  q.rotation = QuantizeQuat(object.rotation);
  return q;
}

WorldObject FromWire(const ObjectQ& q) {
  WorldObject object;
  object.kind = q.kind;
  object.id = q.id;
  object.at_rest = q.at_rest;
  for (int axis = 0; axis < 3; ++axis) {
    object.position[axis] = DequantizePosition(q.position[axis]);
    object.linear_velocity[axis] = DequantizeVelocity(q.linear_velocity[axis]);
    object.angular_velocity[axis] =
        DequantizeVelocity(q.angular_velocity[axis]);
  }
  object.rotation = DequantizeQuat(q.rotation);
  return object;
}

void WriteVelocity(const std::array<int32_t, 3>& velocity, BitWriter& writer) {
  for (const int32_t c : velocity) {
    writer.WriteBoundedI32(c, kVelocityMin, kVelocityMax);
  }
}

std::array<int32_t, 3> ReadVelocity(BitReader& reader) {
  std::array<int32_t, 3> velocity = {};
  for (int32_t& c : velocity) {
    c = reader.ReadBoundedI32(kVelocityMin, kVelocityMax);
  }
  return velocity;
}

}  // namespace

WorldObject Quantized(const WorldObject& object) {
  return FromWire(ToWire(object));
}

void WriteWorldObject(const WorldObject& object, BitWriter& writer) {
  const ObjectQ q = ToWire(object);
  writer.WriteBool(q.kind == ObjectKind::kDroppedWeapon);
  writer.WriteVarUint(q.id);
  writer.WriteBool(q.at_rest);
  for (const int32_t c : q.position) {
    writer.WriteBoundedI32(c, kPositionMin, kPositionMax);
  }
  writer.WriteBits(q.rotation.largest, kLargestIndexBits);
  for (const uint32_t c : q.rotation.components) {
    writer.WriteBoundedU32(c, 0, kQuatComponentMax);
  }
  if (q.at_rest) {
    return;
  }
  WriteVelocity(q.linear_velocity, writer);
  WriteVelocity(q.angular_velocity, writer);
}

WorldObject ReadWorldObject(BitReader& reader) {
  ObjectQ q;
  q.kind = reader.ReadBool() ? ObjectKind::kDroppedWeapon : ObjectKind::kBody;
  q.id = reader.ReadVarUint();
  q.at_rest = reader.ReadBool();
  for (int32_t& c : q.position) {
    c = reader.ReadBoundedI32(kPositionMin, kPositionMax);
  }
  q.rotation.largest = static_cast<uint8_t>(reader.ReadBits(kLargestIndexBits));
  for (uint32_t& c : q.rotation.components) {
    c = reader.ReadBoundedU32(0, kQuatComponentMax);
  }
  if (!q.at_rest) {
    q.linear_velocity = ReadVelocity(reader);
    q.angular_velocity = ReadVelocity(reader);
  }
  return FromWire(q);
}

}  // namespace blue::net
