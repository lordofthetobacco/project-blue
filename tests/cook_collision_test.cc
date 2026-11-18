#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "cook/collision_cook.h"
#include "physics/physics_runtime.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"

namespace blue::cook {
namespace {

using physics::CookedShape;

// A prefab holding one unit cube mesh moved to `offset` by its node.
prefab::Prefab CubePrefab(glm::vec3 offset) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {{.name = "cube", .rest = {.translation = offset}}};
  const asset::MeshData cube = asset::MakeCube();
  prefab::Mesh mesh{.name = "cube",
                    .node = 0,
                    .bounds = cube.bounds,
                    .submeshes = cube.submeshes};
  mesh.vertices = prefab.storage.StoreArray<asset::Vertex>(cube.vertices);
  mesh.indices = prefab.storage.StoreArray<asset::Index>(cube.indices);
  prefab.meshes.push_back(std::move(mesh));
  prefab.materials.push_back({.name = "grey"});
  prefab.bounds = {.min = offset - glm::vec3(0.5f),
                   .max = offset + glm::vec3(0.5f)};
  return prefab;
}

// Restores `bytes` and casts a ray down from above `x`, `z`.
std::optional<physics::ShapeRayHit> CastDown(std::span<const std::byte> bytes,
                                             float x, float z) {
  auto shape = CookedShape::Restore(bytes);
  REQUIRE(shape.ok());
  return shape.value().CastRay({x, 10.0f, z}, {0.0f, -1.0f, 0.0f}, 100.0f);
}

TEST_CASE("Every collider kind cooks and restores with the right surface") {
  physics::PhysicsRuntime runtime;
  const glm::vec3 offset(2.0f, 1.0f, 0.0f);
  for (const prefab::ColliderKind kind :
       {prefab::ColliderKind::kBox, prefab::ColliderKind::kSphere,
        prefab::ColliderKind::kCapsule, prefab::ColliderKind::kConvexHull,
        prefab::ColliderKind::kTriangleMesh}) {
    CAPTURE(static_cast<int>(kind));
    prefab::Prefab prefab = CubePrefab(offset);
    REQUIRE(CookCollision(prefab, {.kind = kind}).ok());
    REQUIRE(!prefab.physics.cooked_shape.empty());
    CHECK(prefab.physics.kind == kind);
    const auto hit = CastDown(prefab.physics.cooked_shape, offset.x, offset.z);
    REQUIRE(hit.has_value());
    // Every fitted shape's top is the cube's top at y = 1.5 (Jolt's convex
    // radius rounds box and hull edges, not the centre of a face).
    CHECK(hit->point.y == doctest::Approx(1.5f).epsilon(0.001));
    CHECK(hit->normal.y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(prefab::ValidatePrefab(prefab) == std::nullopt);
  }
}

TEST_CASE("Fitted primitives take their size from the bounds") {
  physics::PhysicsRuntime runtime;
  prefab::Prefab prefab = CubePrefab(glm::vec3(0.0f));
  REQUIRE(CookCollision(prefab, {.kind = prefab::ColliderKind::kBox}).ok());
  CHECK(prefab.physics.half_extents.x == doctest::Approx(0.5f));
  REQUIRE(CookCollision(prefab, {.kind = prefab::ColliderKind::kSphere}).ok());
  CHECK(prefab.physics.radius == doctest::Approx(0.5f));
  REQUIRE(CookCollision(prefab, {.kind = prefab::ColliderKind::kCapsule}).ok());
  CHECK(prefab.physics.radius == doctest::Approx(0.5f));

  // Explicit parameters are used as given.
  REQUIRE(CookCollision(prefab,
                        {.kind = prefab::ColliderKind::kBox,
                         .center = {0.0f, 3.0f, 0.0f},
                         .half_extents = {1.0f, 0.25f, 1.0f}},
                        ShapeFit::kParameters)
              .ok());
  const auto hit = CastDown(prefab.physics.cooked_shape, 0.0f, 0.0f);
  REQUIRE(hit.has_value());
  CHECK(hit->point.y == doctest::Approx(3.25f));
}

TEST_CASE(
    "The cooked shape survives a .pbo round trip and hits the same "
    "point") {
  physics::PhysicsRuntime runtime;
  prefab::Prefab prefab = CubePrefab(glm::vec3(0.0f, 0.5f, 0.0f));
  REQUIRE(CookCollision(prefab, {.kind = prefab::ColliderKind::kTriangleMesh,
                                 .friction = 0.8f})
              .ok());
  const auto before = CastDown(prefab.physics.cooked_shape, 0.2f, -0.3f);
  REQUIRE(before.has_value());

  auto encoded = prefab::EncodePrefab(prefab, "");
  REQUIRE(encoded.ok());
  auto decoded = prefab::DecodePrefab(std::move(encoded).value(), "");
  REQUIRE(decoded.ok());
  const prefab::Physics& physics = decoded.value().physics;
  CHECK(physics.kind == prefab::ColliderKind::kTriangleMesh);
  CHECK(physics.friction == doctest::Approx(0.8f));
  const auto after = CastDown(physics.cooked_shape, 0.2f, -0.3f);
  REQUIRE(after.has_value());
  CHECK(after->point.x == doctest::Approx(before->point.x));
  CHECK(after->point.y == doctest::Approx(before->point.y));
  CHECK(after->point.z == doctest::Approx(before->point.z));
  CHECK(after->point.y == doctest::Approx(1.0f));
}

TEST_CASE("Corrupt, truncated or foreign cooked shapes do not restore") {
  physics::PhysicsRuntime runtime;
  auto cooked = physics::CookShape(
      {.kind = physics::CookedShapeKind::kSphere, .radius = 1.0f});
  REQUIRE(cooked.ok());
  const std::vector<std::byte>& bytes = cooked.value();
  REQUIRE(CookedShape::Restore(bytes).ok());

  std::vector<std::byte> flipped = bytes;
  flipped.back() ^= std::byte{0x40};
  CHECK(!CookedShape::Restore(flipped).ok());

  CHECK(!CookedShape::Restore(std::span(bytes).first(bytes.size() - 1)).ok());
  CHECK(!CookedShape::Restore(std::span(bytes).first(8)).ok());
  CHECK(!CookedShape::Restore({}).ok());

  std::vector<std::byte> other_version = bytes;
  other_version[4] ^= std::byte{0x01};  // Jolt version field
  CHECK(!CookedShape::Restore(other_version).ok());
}

TEST_CASE("Invalid collision settings leave the prefab unchanged") {
  physics::PhysicsRuntime runtime;
  prefab::Prefab prefab = CubePrefab(glm::vec3(0.0f));
  CHECK(CookCollision(prefab, {.kind = prefab::ColliderKind::kTriangleMesh,
                               .dynamic = true})
            .error() == CookError::kInvalidArgument);
  CHECK(CookCollision(prefab,
                      {.kind = prefab::ColliderKind::kBox,
                       .half_extents = {-1.0f, 1.0f, 1.0f}},
                      ShapeFit::kParameters)
            .error() == CookError::kInvalidArgument);
  CHECK(prefab.physics.kind == prefab::ColliderKind::kNone);
  CHECK(prefab.physics.cooked_shape.empty());

  prefab::Prefab empty;
  CHECK(CookCollision(empty, {.kind = prefab::ColliderKind::kConvexHull})
            .error() == CookError::kInvalidArgument);

  REQUIRE(CookCollision(prefab, {.kind = prefab::ColliderKind::kBox}).ok());
  REQUIRE(CookCollision(prefab, {.kind = prefab::ColliderKind::kNone}).ok());
  CHECK(prefab.physics.kind == prefab::ColliderKind::kNone);
  CHECK(prefab.physics.cooked_shape.empty());
}

}  // namespace
}  // namespace blue::cook
