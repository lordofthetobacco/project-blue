#include "physics/shape_cooking.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_geometric.hpp>
#include <glm/geometric.hpp>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/StreamIn.h>
#include <Jolt/Core/StreamOut.h>
#include <Jolt/Core/UnorderedMap.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/PhysicsMaterial.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>
// clang-format on

#include "core/log.h"
#include "physics/internal/cooked_shape_access.h"
#include "physics/internal/jolt_math.h"
#include "scene/binary_stream.h"

namespace blue::physics {
namespace {

using internal::FromJolt;
using internal::ToJolt;

// Cooked bytes: magic, Jolt version, payload size, payload CRC-32, payload
// (Jolt's Shape::SaveWithChildren stream). The CRC keeps corrupt data away
// from Jolt's reader, which trusts array lengths.
constexpr uint32_t kMagic = scene::MakeFourcc('B', 'J', 'S', 'H');
constexpr uint32_t kJoltVersion =
    (JPH_VERSION_MAJOR << 16) | (JPH_VERSION_MINOR << 8) | JPH_VERSION_PATCH;
constexpr size_t kHeaderSize = 16;

// Jolt output stream appending to a byte vector.
class VectorStreamOut final : public JPH::StreamOut {
 public:
  explicit VectorStreamOut(std::vector<std::byte>& bytes) : bytes_(bytes) {}

  void WriteBytes(const void* data, size_t size) override {
    const auto* first = static_cast<const std::byte*>(data);
    bytes_.insert(bytes_.end(), first, first + size);
  }
  bool IsFailed() const override { return false; }

 private:
  std::vector<std::byte>& bytes_;
};

// Jolt input stream over a byte span; reading past the end fails and yields
// zeros.
class SpanStreamIn final : public JPH::StreamIn {
 public:
  explicit SpanStreamIn(std::span<const std::byte> bytes) : bytes_(bytes) {}

  void ReadBytes(void* data, size_t size) override {
    if (failed_ || size > bytes_.size() - position_) {
      failed_ = true;
      std::memset(data, 0, size);
      return;
    }
    std::memcpy(data, bytes_.data() + position_, size);
    position_ += size;
  }
  // Like std::istream::eof(): set only by a read that ran past the end, not
  // by consuming the last byte (Jolt checks it after every read).
  bool IsEOF() const override { return failed_; }
  bool IsFailed() const override { return failed_; }

 private:
  std::span<const std::byte> bytes_;
  size_t position_ = 0;
  bool failed_ = false;
};

using ShapeResult = core::Result<JPH::ShapeRefC, PhysicsError>;

bool IsFinite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

ShapeResult FromSettings(const JPH::ShapeSettings::ShapeResult& created) {
  if (created.HasError()) {
    core::log::Warn("CookShape: shape rejected: {}",
                    created.GetError().c_str());
    return ShapeResult::Error(PhysicsError::kShapeBuildFailed);
  }
  return ShapeResult::Ok(JPH::ShapeRefC(created.Get()));
}

// Returns `shape` placed at `center` / `rotation`, or `shape` itself for the
// identity placement.
ShapeResult Place(const JPH::ShapeRefC& shape, glm::vec3 center,
                  const glm::quat& rotation) {
  const bool identity_rotation =
      std::abs(std::abs(rotation.w) - 1.0f) < 1.0e-7f;
  if (glm::dot(center, center) < 1.0e-12f && identity_rotation) {
    return ShapeResult::Ok(shape);
  }
  JPH::RotatedTranslatedShapeSettings settings(
      ToJolt(center), ToJolt(glm::normalize(rotation)), shape.GetPtr());
  return FromSettings(settings.Create());
}

ShapeResult BuildPrimitive(const ShapeCookDesc& desc) {
  if (!IsFinite(desc.center) || !std::isfinite(desc.rotation.w) ||
      !std::isfinite(desc.rotation.x) || !std::isfinite(desc.rotation.y) ||
      !std::isfinite(desc.rotation.z) || glm::length(desc.rotation) < 0.5f) {
    return ShapeResult::Error(PhysicsError::kInvalidShape);
  }
  ShapeResult base = ShapeResult::Error(PhysicsError::kInvalidShape);
  switch (desc.kind) {
    case CookedShapeKind::kBox: {
      const glm::vec3 half = desc.half_extents;
      if (!IsFinite(half) || half.x <= 0.0f || half.y <= 0.0f ||
          half.z <= 0.0f) {
        return ShapeResult::Error(PhysicsError::kInvalidShape);
      }
      const float convex_radius = std::min(
          JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
      JPH::BoxShapeSettings settings(ToJolt(half), convex_radius);
      base = FromSettings(settings.Create());
      break;
    }
    case CookedShapeKind::kSphere: {
      if (!std::isfinite(desc.radius) || desc.radius <= 0.0f) {
        return ShapeResult::Error(PhysicsError::kInvalidShape);
      }
      JPH::SphereShapeSettings settings(desc.radius);
      base = FromSettings(settings.Create());
      break;
    }
    case CookedShapeKind::kCapsule: {
      if (!std::isfinite(desc.radius) || desc.radius <= 0.0f ||
          !std::isfinite(desc.half_height) || desc.half_height <= 0.0f) {
        return ShapeResult::Error(PhysicsError::kInvalidShape);
      }
      JPH::CapsuleShapeSettings settings(desc.half_height, desc.radius);
      base = FromSettings(settings.Create());
      break;
    }
    case CookedShapeKind::kConvexHull:
    case CookedShapeKind::kTriangleMesh:
      break;
  }
  if (!base.ok()) {
    return base;
  }
  return Place(base.value(), desc.center, desc.rotation);
}

ShapeResult BuildConvexHull(std::span<const glm::vec3> points) {
  if (points.size() < 4) {
    return ShapeResult::Error(PhysicsError::kInvalidShape);
  }
  JPH::Array<JPH::Vec3> jolt_points;
  jolt_points.reserve(points.size());
  for (const glm::vec3& point : points) {
    if (!IsFinite(point)) {
      return ShapeResult::Error(PhysicsError::kInvalidShape);
    }
    jolt_points.push_back(ToJolt(point));
  }
  JPH::ConvexHullShapeSettings settings(jolt_points);
  return FromSettings(settings.Create());
}

ShapeResult BuildTriangleMesh(std::span<const glm::vec3> points,
                              std::span<const uint32_t> indices) {
  if (points.empty() || indices.empty() || indices.size() % 3 != 0) {
    return ShapeResult::Error(PhysicsError::kInvalidShape);
  }
  JPH::VertexList vertices;
  vertices.reserve(points.size());
  for (const glm::vec3& point : points) {
    if (!IsFinite(point)) {
      return ShapeResult::Error(PhysicsError::kInvalidShape);
    }
    vertices.push_back(JPH::Float3(point.x, point.y, point.z));
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(indices.size() / 3);
  for (size_t i = 0; i < indices.size(); i += 3) {
    if (indices[i] >= points.size() || indices[i + 1] >= points.size() ||
        indices[i + 2] >= points.size()) {
      return ShapeResult::Error(PhysicsError::kInvalidShape);
    }
    triangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1],
                                             indices[i + 2],
                                             /*inMaterialIndex=*/0));
  }
  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  return FromSettings(settings.Create());
}

ShapeResult BuildShape(const ShapeCookDesc& desc) {
  switch (desc.kind) {
    case CookedShapeKind::kBox:
    case CookedShapeKind::kSphere:
    case CookedShapeKind::kCapsule:
      return BuildPrimitive(desc);
    case CookedShapeKind::kConvexHull:
      return BuildConvexHull(desc.points);
    case CookedShapeKind::kTriangleMesh:
      return BuildTriangleMesh(desc.points, desc.triangle_indices);
  }
  return ShapeResult::Error(PhysicsError::kInvalidShape);
}

}  // namespace

core::Result<std::vector<std::byte>, PhysicsError> CookShape(
    const ShapeCookDesc& desc) {
  using Result = core::Result<std::vector<std::byte>, PhysicsError>;
  ShapeResult shape = BuildShape(desc);
  if (!shape.ok()) {
    return Result::Error(shape.error());
  }
  std::vector<std::byte> payload;
  VectorStreamOut stream(payload);
  JPH::Shape::ShapeToIDMap shape_map;
  JPH::Shape::MaterialToIDMap material_map;
  shape.value()->SaveWithChildren(stream, shape_map, material_map);

  scene::BinaryWriter writer;
  writer.WriteU32(kMagic);
  writer.WriteU32(kJoltVersion);
  writer.WriteU32(static_cast<uint32_t>(payload.size()));
  writer.WriteU32(scene::Crc32(payload));
  writer.WriteBytes(payload);
  return Result::Ok(writer.bytes());
}

CookedShape::CookedShape(std::shared_ptr<const Holder> holder)
    : holder_(std::move(holder)) {}

CookedShape::~CookedShape() = default;

core::Result<CookedShape, PhysicsError> CookedShape::Restore(
    std::span<const std::byte> bytes) {
  using Result = core::Result<CookedShape, PhysicsError>;
  scene::BinaryReader reader(bytes);
  const uint32_t magic = reader.ReadU32();
  const uint32_t version = reader.ReadU32();
  const uint32_t size = reader.ReadU32();
  const uint32_t crc = reader.ReadU32();
  if (reader.status() != scene::ReadStatus::kOk || magic != kMagic) {
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  if (version != kJoltVersion) {
    core::log::Warn(
        "CookedShape: cooked with Jolt {}.{}.{}, this build uses {}.{}.{}; "
        "re-cook the prefab",
        version >> 16, (version >> 8) & 0xff, version & 0xff, JPH_VERSION_MAJOR,
        JPH_VERSION_MINOR, JPH_VERSION_PATCH);
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  const std::span<const std::byte> payload = reader.ReadBytes(size);
  if (reader.status() != scene::ReadStatus::kOk ||
      bytes.size() != kHeaderSize + size || scene::Crc32(payload) != crc) {
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  SpanStreamIn stream(payload);
  JPH::Shape::IDToShapeMap shape_map;
  JPH::Shape::IDToMaterialMap material_map;
  const JPH::Shape::ShapeResult restored =
      JPH::Shape::sRestoreWithChildren(stream, shape_map, material_map);
  if (restored.HasError() || stream.IsFailed() || !restored.IsValid()) {
    core::log::Warn(
        "CookedShape: restore failed: {}",
        restored.HasError() ? restored.GetError().c_str() : "truncated data");
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  return Result::Ok(CookedShape(std::make_shared<const Holder>(
      Holder{.shape = JPH::ShapeRefC(restored.Get())})));
}

std::optional<ShapeRayHit> CookedShape::CastRay(glm::vec3 origin,
                                                glm::vec3 direction,
                                                float max_distance) const {
  const float length = glm::length(direction);
  if (!(length > 0.0f) || !(max_distance > 0.0f) || !IsFinite(origin)) {
    return std::nullopt;
  }
  const JPH::Shape& shape = *holder_->shape;
  // Shape queries work relative to the centre of mass.
  const JPH::Vec3 center_of_mass = shape.GetCenterOfMass();
  const glm::vec3 unit = direction / length;
  const JPH::RayCast ray{ToJolt(origin) - center_of_mass,
                         ToJolt(unit * max_distance)};
  JPH::RayCastResult hit;
  if (!shape.CastRay(ray, JPH::SubShapeIDCreator(), hit)) {
    return std::nullopt;
  }
  const JPH::Vec3 local_point = ray.GetPointOnRay(hit.mFraction);
  const JPH::Vec3 normal =
      shape.GetSurfaceNormal(hit.mSubShapeID2, local_point).Normalized();
  return ShapeRayHit{.distance = hit.mFraction * max_distance,
                     .point = FromJolt(local_point + center_of_mass),
                     .normal = FromJolt(normal)};
}

asset::Bounds CookedShape::bounds() const {
  const JPH::Shape& shape = *holder_->shape;
  const JPH::AABox box = shape.GetLocalBounds();
  const JPH::Vec3 center_of_mass = shape.GetCenterOfMass();
  return {.min = FromJolt(box.mMin + center_of_mass),
          .max = FromJolt(box.mMax + center_of_mass)};
}

}  // namespace blue::physics
