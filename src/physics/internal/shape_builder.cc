#include "physics/internal/shape_builder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
// clang-format on

#include "core/log.h"

namespace blue::physics::internal {
namespace {

using Result = core::Result<JPH::ShapeRefC, PhysicsError>;

// Axes thinner than kMinBoxThickness get this half extent.
constexpr float kThinHalfExtent = 0.5f * kMinBoxThickness;

bool IsFinite(glm::vec3 v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

JPH::Vec3 ToJolt(glm::vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }

// Returns `shape` offset by `offset`, or `shape` itself when the offset is
// negligible.
JPH::ShapeRefC Offset(JPH::ShapeRefC shape, glm::vec3 offset) {
  if (glm::dot(offset, offset) < 1.0e-12f) {
    return shape;
  }
  return new JPH::RotatedTranslatedShape(ToJolt(offset), JPH::Quat::sIdentity(),
                                         shape);
}

Result FromSettings(const JPH::ShapeSettings::ShapeResult& created) {
  if (created.HasError()) {
    core::log::Warn("Physics shape rejected: {}", created.GetError().c_str());
    return Result::Error(PhysicsError::kShapeBuildFailed);
  }
  return Result::Ok(JPH::ShapeRefC(created.Get()));
}

bool BoundsAreValid(const asset::Bounds& bounds) {
  const glm::vec3 size = bounds.size();
  return IsFinite(bounds.min) && IsFinite(bounds.max) && size.x >= 0.0f &&
         size.y >= 0.0f && size.z >= 0.0f;
}

// Builds a box fitted to `bounds`. Thin axes are thickened downwards so the
// top face stays where the original surface was.
Result BuildBox(const asset::Bounds& bounds) {
  const glm::vec3 size = bounds.size();
  glm::vec3 half = size * 0.5f;
  glm::vec3 center = bounds.center();
  for (int axis = 0; axis < 3; ++axis) {
    if (size[axis] < kMinBoxThickness) {
      half[axis] = kThinHalfExtent;
      center[axis] = bounds.max[axis] - kThinHalfExtent;
    }
  }
  const float convex_radius = std::min(
      JPH::cDefaultConvexRadius, 0.5f * std::min({half.x, half.y, half.z}));
  JPH::BoxShapeSettings settings(ToJolt(half), convex_radius);
  Result box = FromSettings(settings.Create());
  if (!box.ok()) {
    return box;
  }
  return Result::Ok(Offset(box.value(), center));
}

Result BuildSphere(const asset::Bounds& bounds) {
  const glm::vec3 half = bounds.size() * 0.5f;
  const float radius = std::max({half.x, half.y, half.z});
  if (radius <= 0.0f) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  JPH::SphereShapeSettings settings(radius);
  Result sphere = FromSettings(settings.Create());
  if (!sphere.ok()) {
    return sphere;
  }
  return Result::Ok(Offset(sphere.value(), bounds.center()));
}

Result BuildConvexHull(const asset::MeshData& mesh) {
  if (mesh.vertices.empty()) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  JPH::Array<JPH::Vec3> points;
  points.reserve(mesh.vertices.size());
  for (const asset::Vertex& vertex : mesh.vertices) {
    points.push_back(ToJolt(vertex.position));
  }
  JPH::ConvexHullShapeSettings settings(points);
  return FromSettings(settings.Create());
}

Result BuildTriangleMesh(const asset::MeshData& mesh) {
  if (mesh.vertices.empty() || mesh.indices.empty() ||
      mesh.indices.size() % 3 != 0) {
    return Result::Error(PhysicsError::kInvalidShape);
  }
  const size_t vertex_count = mesh.vertices.size();
  JPH::VertexList vertices;
  vertices.reserve(vertex_count);
  for (const asset::Vertex& vertex : mesh.vertices) {
    vertices.push_back(
        JPH::Float3(vertex.position.x, vertex.position.y, vertex.position.z));
  }
  JPH::IndexedTriangleList triangles;
  triangles.reserve(mesh.indices.size() / 3);
  for (size_t i = 0; i < mesh.indices.size(); i += 3) {
    for (size_t corner = 0; corner < 3; ++corner) {
      if (mesh.indices[i + corner] >= vertex_count) {
        return Result::Error(PhysicsError::kInvalidShape);
      }
    }
    triangles.push_back(JPH::IndexedTriangle(
        mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2],
        /*material_index=*/0));
  }
  JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
  return FromSettings(settings.Create());
}

}  // namespace

Result BuildBaseShape(const BodyDesc& desc) {
  switch (desc.body.shape) {
    case scene::ShapeType::kBox:
      return BoundsAreValid(desc.bounds)
                 ? BuildBox(desc.bounds)
                 : Result::Error(PhysicsError::kInvalidShape);
    case scene::ShapeType::kSphere:
      return BoundsAreValid(desc.bounds)
                 ? BuildSphere(desc.bounds)
                 : Result::Error(PhysicsError::kInvalidShape);
    case scene::ShapeType::kConvexHull:
      return desc.mesh != nullptr ? BuildConvexHull(*desc.mesh)
                                  : Result::Error(PhysicsError::kInvalidShape);
    case scene::ShapeType::kTriangleMesh:
      if (desc.body.motion == scene::MotionType::kDynamic) {
        return Result::Error(PhysicsError::kDynamicTriangleMesh);
      }
      return desc.mesh != nullptr ? BuildTriangleMesh(*desc.mesh)
                                  : Result::Error(PhysicsError::kInvalidShape);
  }
  return Result::Error(PhysicsError::kInvalidShape);
}

Result ApplyScale(const JPH::ShapeRefC& base, glm::vec3 scale) {
  if (scale == glm::vec3(1.0f)) {
    return Result::Ok(base);
  }
  JPH::Vec3 valid_scale = base->MakeScaleValid(ToJolt(scale));
  JPH::ScaledShapeSettings settings(base.GetPtr(), valid_scale);
  return FromSettings(settings.Create());
}

}  // namespace blue::physics::internal
