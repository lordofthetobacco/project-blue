#include "weapon/weapon_geometry.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/animated_model.h"
#include "asset/mesh_data.h"
#include "asset/model_importer.h"
#include "core/path.h"
#include "core/result.h"

namespace blue::weapon {
namespace {

// Returns the M16 as authored in its MTL file (sidecar overrides off).
asset::Model ImportM16Model() {
  core::Result<asset::Model, asset::ImportError> model =
      asset::ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "m16.obj"),
                         {.use_material_overrides = false});
  REQUIRE(model.ok());
  return std::move(model).value();
}

asset::MeshData ImportM16() { return ImportM16Model().mesh; }

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
  const WeaponGeometry& rifle = M16Geometry();
  CHECK(Inside(bounds, rifle.muzzle));
  CHECK(Inside(bounds, rifle.sight_eye_point));
  CHECK(Inside(bounds, rifle.optic_lens));
  CHECK(Inside(bounds, rifle.grip));
  // The muzzle is the front end of the model.
  CHECK(rifle.muzzle.z == doctest::Approx(bounds.max.z).epsilon(1.0e-3));
  const float length = (bounds.max.z - bounds.min.z) * rifle.scale;
  CHECK(length >= 1.0f);
  CHECK(length <= 1.25f);

  // The optic axis runs through the middle of the hood's window. Nothing
  // ahead blocks it, nor lines 7.7 mm (0.1 units) above, below or beside it
  // (the old A2 front sight blocked everything at x = 0 below y 1.412); 10 mm
  // (0.13 units) above or below it the hood's roof and floor do.
  const glm::vec3 up(0.0f, 1.0f, 0.0f);
  const glm::vec3 side(1.0f, 0.0f, 0.0f);
  for (const glm::vec3 offset :
       {glm::vec3(0.0f), 0.1f * up, -0.1f * up, 0.1f * side, -0.1f * side}) {
    CAPTURE(offset.x);
    CAPTURE(offset.y);
    CHECK_FALSE(
        RayHitsMesh(mesh, rifle.sight_eye_point + offset, rifle.sight_axis));
  }
  CHECK(
      RayHitsMesh(mesh, rifle.sight_eye_point + 0.13f * up, rifle.sight_axis));
  CHECK(
      RayHitsMesh(mesh, rifle.sight_eye_point - 0.13f * up, rifle.sight_axis));
  // The sight line is centred in the window, level with the lens.
  CHECK(rifle.sight_eye_point.y == doctest::Approx(rifle.optic_lens.y));
}

TEST_CASE("the M16 optic sits above the bore, the lens ahead of the eye") {
  const WeaponGeometry& rifle = M16Geometry();
  CHECK(glm::length(rifle.sight_axis) == doctest::Approx(1.0f));
  CHECK(rifle.sight_axis.z > 0.99f);
  CHECK(rifle.optic_lens.z > rifle.sight_eye_point.z);
  CHECK(rifle.muzzle.z > rifle.optic_lens.z);
  CHECK(rifle.grip.y < rifle.muzzle.y);
  CHECK(rifle.grip.z < rifle.sight_eye_point.z);
  // The axis passes through the lens disc.
  const glm::vec3 to_lens = rifle.optic_lens - rifle.sight_eye_point;
  const glm::vec3 across =
      to_lens - glm::dot(to_lens, rifle.sight_axis) * rifle.sight_axis;
  CHECK(glm::length(across) < rifle.optic_lens_radius);
  // Lens 1.5 .. 3 cm across; axis 5 .. 8 cm above the bore.
  CHECK(2.0f * rifle.optic_lens_radius * rifle.scale > 0.015f);
  CHECK(2.0f * rifle.optic_lens_radius * rifle.scale < 0.03f);
  const float height = (rifle.sight_eye_point.y - rifle.muzzle.y) * rifle.scale;
  CHECK(height > 0.05f);
  CHECK(height < 0.08f);
}

TEST_CASE("the M16 materials arrive with their own dark colours") {
  const asset::Model model = ImportM16Model();
  std::set<std::tuple<float, float, float>> colors;
  float brightest = 0.0f;
  for (const asset::Submesh& submesh : model.mesh.submeshes) {
    const glm::vec4& color =
        model.materials[static_cast<size_t>(submesh.material)].base_color;
    colors.emplace(color.r, color.g, color.b);
    brightest = std::max({brightest, color.r, color.g, color.b});
  }
  // Dark gun finishes, not the 0.8 grey fallback.
  CHECK(brightest < 0.25f);
  // Grip and stock share black polymer: four distinct colours.
  CHECK(colors.size() == 4);
  CHECK(colors.count({0.105f, 0.105f, 0.11f}) == 1);  // parkerized steel
}

// Returns the Glock's parts at the rest pose as model-space meshes.
std::vector<asset::MeshData> ImportGlockAtRest(asset::Bounds& rest_bounds) {
  core::Result<asset::AnimatedModel, asset::ImportError> imported =
      asset::ImportAnimatedModel(
          core::JoinPath(BLUE_SAMPLES_DIR, "glock/glock17.glb"));
  REQUIRE(imported.ok());
  const asset::AnimatedModel model = std::move(imported).value();
  rest_bounds = model.rest_bounds;
  std::vector<animation::Transform> pose(model.hierarchy.nodes.size());
  animation::ResetToRest(model.hierarchy, pose);
  std::vector<glm::mat4> world(pose.size());
  animation::ComputeWorldMatrices(model.hierarchy, pose, world);
  std::vector<asset::MeshData> meshes;
  for (const asset::ModelPart& part : model.parts) {
    asset::MeshData mesh = part.mesh;
    for (asset::Vertex& vertex : mesh.vertices) {
      vertex.position = glm::vec3(world[static_cast<size_t>(part.node)] *
                                  glm::vec4(vertex.position, 1.0f));
    }
    meshes.push_back(std::move(mesh));
  }
  return meshes;
}

bool RayHitsAny(const std::vector<asset::MeshData>& meshes, glm::vec3 origin,
                glm::vec3 direction) {
  return std::any_of(meshes.begin(), meshes.end(),
                     [&](const asset::MeshData& mesh) {
                       return RayHitsMesh(mesh, origin, direction);
                     });
}

TEST_CASE("the Glock points lie in the model, which is about 0.2 m long") {
  asset::Bounds bounds;
  const std::vector<asset::MeshData> meshes = ImportGlockAtRest(bounds);
  const WeaponGeometry& pistol = Glock17Geometry();
  CHECK(Inside(bounds, pistol.muzzle));
  CHECK(Inside(bounds, pistol.sight_eye_point));
  CHECK(Inside(bounds, pistol.grip));
  // The muzzle is the front (-X) end of the model.
  CHECK(pistol.muzzle.x == doctest::Approx(bounds.min.x).epsilon(1.0e-3));
  const float length = (bounds.max.x - bounds.min.x) * pistol.scale;
  CHECK(length > 0.19f);
  CHECK(length < 0.21f);

  // The sight line, just above the notch's top, clears the rear sight and
  // the front post; 1.5 mm lower (inside the notch) it hits the front post.
  const glm::vec3 above =
      pistol.sight_eye_point + glm::vec3(0.0f, 0.003f, 0.0f);
  CHECK_FALSE(RayHitsAny(meshes, above, pistol.sight_axis));
  const glm::vec3 low = pistol.sight_eye_point - glm::vec3(0.0f, 0.018f, 0.0f);
  CHECK(RayHitsAny(meshes, low, pistol.sight_axis));
}

TEST_CASE("the Glock has iron sights above the bore along -X") {
  const WeaponGeometry& pistol = Glock17Geometry();
  CHECK(pistol.optic_lens_radius == 0.0f);
  CHECK(glm::length(pistol.sight_axis) == doctest::Approx(1.0f));
  CHECK(pistol.sight_axis.x < -0.99f);
  CHECK(pistol.muzzle.x < pistol.sight_eye_point.x);
  CHECK(pistol.grip.y < pistol.muzzle.y);
  CHECK(pistol.grip.x > pistol.muzzle.x);
  // Sight line 1 .. 2.5 cm above the bore; scale from a 0.186 m slide.
  const float height =
      (pistol.sight_eye_point.y - pistol.muzzle.y) * pistol.scale;
  CHECK(height > 0.01f);
  CHECK(height < 0.025f);
  CHECK(2.210f * pistol.scale == doctest::Approx(0.186f).epsilon(0.005));
}

}  // namespace
}  // namespace blue::weapon
