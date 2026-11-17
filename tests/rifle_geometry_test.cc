#include "weapon/rifle_geometry.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <tuple>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "asset/mesh_data.h"
#include "asset/model_importer.h"
#include "core/path.h"
#include "core/result.h"

namespace blue::weapon {
namespace {

asset::MeshData ImportM16() {
  core::Result<asset::MeshData, asset::ImportError> mesh =
      asset::ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "m16.obj"));
  REQUIRE(mesh.ok());
  return std::move(mesh).value();
}

// Returns true when the ray `origin` + t * `direction`, t > 0, crosses any
// triangle of `mesh` (Moller-Trumbore, both faces).
bool RayHitsMesh(const asset::MeshData& mesh, glm::vec3 origin,
                 glm::vec3 direction) {
  constexpr float kParallel = 1.0e-9f;
  for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    const glm::vec3 a = mesh.vertices[mesh.indices[i]].position;
    const glm::vec3 b = mesh.vertices[mesh.indices[i + 1]].position;
    const glm::vec3 c = mesh.vertices[mesh.indices[i + 2]].position;
    const glm::vec3 edge1 = b - a;
    const glm::vec3 edge2 = c - a;
    const glm::vec3 p = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, p);
    if (std::abs(determinant) < kParallel) {
      continue;
    }
    const glm::vec3 s = origin - a;
    const float u = glm::dot(s, p) / determinant;
    const glm::vec3 q = glm::cross(s, edge1);
    const float v = glm::dot(direction, q) / determinant;
    const float t = glm::dot(edge2, q) / determinant;
    if (u >= 0.0f && v >= 0.0f && u + v <= 1.0f && t > 0.0f) {
      return true;
    }
  }
  return false;
}

bool Inside(const asset::Bounds& bounds, glm::vec3 point) {
  return point.x >= bounds.min.x && point.x <= bounds.max.x &&
         point.y >= bounds.min.y && point.y <= bounds.max.y &&
         point.z >= bounds.min.z && point.z <= bounds.max.z;
}

TEST_CASE("the M16 points lie inside the mesh, which is 1.0 to 1.25 m long") {
  const asset::MeshData mesh = ImportM16();
  const asset::Bounds bounds = mesh.bounds;
  const RifleGeometry& rifle = M16Geometry();
  CHECK(Inside(bounds, rifle.muzzle));
  CHECK(Inside(bounds, rifle.optic_eye_side));
  CHECK(Inside(bounds, rifle.optic_lens));
  CHECK(Inside(bounds, rifle.grip));
  // The muzzle is the front end of the model.
  CHECK(rifle.muzzle.z == doctest::Approx(bounds.max.z).epsilon(1.0e-3));
  const float length = (bounds.max.z - bounds.min.z) * rifle.scale;
  CHECK(length >= 1.0f);
  CHECK(length <= 1.25f);

  // The optic axis looks out of the hood and through the front sight's
  // notch without touching the rifle; 8 mm lower it hits the sight base.
  CHECK_FALSE(RayHitsMesh(mesh, rifle.optic_eye_side, rifle.optic_axis));
  const glm::vec3 low = rifle.optic_eye_side - glm::vec3(0.0f, 0.1f, 0.0f);
  CHECK(RayHitsMesh(mesh, low, rifle.optic_axis));
}

TEST_CASE("the M16 optic sits above the bore, the lens ahead of the eye") {
  const RifleGeometry& rifle = M16Geometry();
  CHECK(glm::length(rifle.optic_axis) == doctest::Approx(1.0f));
  CHECK(rifle.optic_axis.z > 0.99f);
  CHECK(rifle.optic_lens.z > rifle.optic_eye_side.z);
  CHECK(rifle.muzzle.z > rifle.optic_lens.z);
  CHECK(rifle.grip.y < rifle.muzzle.y);
  CHECK(rifle.grip.z < rifle.optic_eye_side.z);
  // The axis passes through the lens disc.
  const glm::vec3 to_lens = rifle.optic_lens - rifle.optic_eye_side;
  const glm::vec3 across =
      to_lens - glm::dot(to_lens, rifle.optic_axis) * rifle.optic_axis;
  CHECK(glm::length(across) < rifle.optic_lens_radius);
  // Lens 1.5 .. 3 cm across; axis 5 .. 8 cm above the bore.
  CHECK(2.0f * rifle.optic_lens_radius * rifle.scale > 0.015f);
  CHECK(2.0f * rifle.optic_lens_radius * rifle.scale < 0.03f);
  const float height = (rifle.optic_eye_side.y - rifle.muzzle.y) * rifle.scale;
  CHECK(height > 0.05f);
  CHECK(height < 0.08f);
}

TEST_CASE("the M16 materials arrive with their own dark colours") {
  const asset::MeshData mesh = ImportM16();
  std::set<std::tuple<float, float, float>> colors;
  float brightest = 0.0f;
  for (const asset::Vertex& vertex : mesh.vertices) {
    colors.emplace(vertex.color.r, vertex.color.g, vertex.color.b);
    brightest =
        std::max({brightest, vertex.color.r, vertex.color.g, vertex.color.b});
  }
  // Dark gun finishes, not the 0.8 grey fallback.
  CHECK(brightest < 0.25f);
  // Grip and stock share black polymer: four distinct colours.
  CHECK(colors.size() == 4);
  CHECK(colors.count({0.105f, 0.105f, 0.11f}) == 1);  // parkerized steel
}

}  // namespace
}  // namespace blue::weapon
