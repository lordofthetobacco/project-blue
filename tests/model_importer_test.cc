#include "asset/model_importer.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>

#include "core/path.h"
#include "material/material.h"
#include "material/material_file.h"
#include "platform/file_system.h"
#include "tests/mesh_checks.h"
#include "tests/scratch_directory.h"

namespace blue::asset {
namespace {

constexpr float kEpsilon = 1.0e-3f;

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b) {
  return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

// Writes `text` to `path`; fails the test if it cannot.
void WriteText(const std::string& path, std::string_view text) {
  REQUIRE(platform::WriteFileAtomic(path, std::as_bytes(std::span(text))).ok());
}

// Returns the material called `name` in `model`, or nullptr.
const material::MaterialDesc* FindMaterial(const Model& model,
                                           std::string_view name) {
  const auto found =
      std::ranges::find(model.materials, name, &material::MaterialDesc::name);
  return found == model.materials.end() ? nullptr : &*found;
}

}  // namespace

TEST_CASE("ImportModel loads the teapot") {
  const auto result =
      ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "teapot.obj"));
  REQUIRE(result.ok());
  const MeshData& mesh = result.value().mesh;

  CHECK(mesh.indices.size() == 18960);
  REQUIRE_FALSE(mesh.vertices.empty());
  // No MTL file: one grey default material.
  REQUIRE(result.value().materials.size() == 1);
  CHECK(result.value().materials[0].base_color ==
        glm::vec4(0.8f, 0.8f, 0.8f, 1.0f));
  testing::CheckSingleSubmesh(mesh);
  testing::CheckTangentFrames(mesh, /*min_agreement=*/0.0f);

  for (const Vertex& vertex : mesh.vertices) {
    CHECK(std::abs(glm::length(vertex.normal) - 1.0f) <= kEpsilon);
    // The file has no UVs.
    CHECK(vertex.uv0 == glm::vec2(0.0f));
  }
  for (const Index index : mesh.indices) {
    CHECK(index < mesh.vertices.size());
  }

  CHECK(NearlyEqual(mesh.bounds.min, {-3.0f, 0.0f, -2.0f}));
  CHECK(NearlyEqual(mesh.bounds.max, {3.434f, 3.15f, 2.0f}));
}

TEST_CASE("ImportModel reports a missing file") {
  const auto result = ImportModel(
      core::JoinPath(platform::TempDirectory(), "blue_no_such_model.obj"));
  REQUIRE_FALSE(result.ok());
  // Parenthesised: doctest would find blue::asset::ToString by ADL.
  CHECK((result.error() == ImportError::kFileNotFound));
}

TEST_CASE("ImportModel triangulates quads and n-gons") {
  const testing::ScratchDirectory dir("polygon_model");
  const std::string path = dir.Child("blue_polygon_model.obj");
  // A unit quad and a regular hexagon (both in z = 0), as Blender exports.
  const std::string_view obj =
      "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
      "v 3 0 0\nv 3.5 0.87 0\nv 3 1.73 0\nv 2 1.73 0\nv 1.5 0.87 0\nv 2 0 0\n"
      "vn 0 0 1\n"
      "f 1//1 2//1 3//1 4//1\n"
      "f 5//1 6//1 7//1 8//1 9//1 10//1\n";
  REQUIRE(platform::WriteFileAtomic(path, std::as_bytes(std::span(obj))).ok());

  const auto result = ImportModel(path);

  REQUIRE(result.ok());
  // 2 + 4 triangles.
  CHECK(result.value().mesh.indices.size() == 18);
}

TEST_CASE("ImportModel rejects a garbage file without crashing") {
  const testing::ScratchDirectory dir("garbage_model");
  const std::string path = dir.Child("blue_garbage_model.obj");
  // Stops at the first NUL, as the old stream write did.
  const std::string_view garbage = "\x01\x02\xff\xfe junk ";
  REQUIRE(
      platform::WriteFileAtomic(path, std::as_bytes(std::span(garbage))).ok());

  const auto result = ImportModel(path);

  REQUIRE_FALSE(result.ok());
  CHECK((result.error() == ImportError::kParseFailed ||
         result.error() == ImportError::kNoMeshes));
}

TEST_CASE("ImportModel handles a non-ASCII directory name") {
  const testing::ScratchDirectory scratch("importer");
  // "modele_" + z-dot o-acute l-stroke w
  const std::string copy =
      scratch.Child("modele_\xC5\xBC\xC3\xB3\xC5\x82w/teapot.obj");
  const auto teapot = platform::ReadFile(
      core::JoinPath(BLUE_SAMPLES_DIR, "teapot.obj"), 16 * 1024 * 1024);
  REQUIRE(teapot.ok());
  REQUIRE(platform::WriteFileAtomic(copy, teapot.value()).ok());

  const auto result = ImportModel(copy);

  REQUIRE(result.ok());
  CHECK(result.value().mesh.indices.size() == 18960);
}

TEST_CASE("ImportModel flips OBJ UVs to top-left and orients tangents") {
  const testing::ScratchDirectory dir("uv_model");
  const std::string path = dir.Child("quad.obj");
  WriteText(dir.Child("albedo.png"), "not decoded at import");
  WriteText(dir.Child("quad.mtl"),
            "newmtl Paint\n"
            "Kd 0.5 0.25 0.125\n"
            "Ns 98\n"
            "map_Kd albedo.png\n"
            "norm missing_normal.png\n");
  // A unit quad facing +Z; OBJ texture v points up the image.
  WriteText(path,
            "mtllib quad.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
            "vn 0 0 1\n"
            "usemtl Paint\n"
            "f 1/1/1 2/2/1 3/3/1 4/4/1\n");

  const auto result = ImportModel(path);

  REQUIRE(result.ok());
  const Model& model = result.value();
  REQUIRE(model.mesh.vertices.size() == 4);
  for (const Vertex& vertex : model.mesh.vertices) {
    // Top-left convention: the top edge (y = 1) has v = 0.
    CHECK(vertex.uv0.x == doctest::Approx(vertex.position.x));
    CHECK(vertex.uv0.y == doctest::Approx(1.0f - vertex.position.y));
    CHECK(NearlyEqual(glm::vec3(vertex.tangent), {1.0f, 0.0f, 0.0f}));
    // The bitangent points along +v, down the image.
    CHECK(NearlyEqual(
        glm::cross(vertex.normal, glm::vec3(vertex.tangent)) * vertex.tangent.w,
        {0.0f, -1.0f, 0.0f}));
  }
  testing::CheckTangentFrames(model.mesh);

  REQUIRE(model.materials.size() == 1);
  const material::MaterialDesc& paint = model.materials[0];
  CHECK(paint.name == "Paint");
  CHECK(paint.base_color == glm::vec4(0.5f, 0.25f, 0.125f, 1.0f));
  CHECK(paint.metallic == 0.0f);
  CHECK(paint.roughness ==
        doctest::Approx(material::RoughnessFromShininess(98.0f)));
  CHECK(core::NormalizePath(paint.base_color_texture.path) ==
        dir.Child("albedo.png"));
  CHECK_FALSE(paint.base_color_texture.embedded());
  // The missing normal map leaves its slot empty.
  CHECK(paint.normal_texture.empty());
}

TEST_CASE("ImportModel groups submeshes by material and merges duplicates") {
  const testing::ScratchDirectory dir("submesh_model");
  const std::string path = dir.Child("parts.obj");
  WriteText(dir.Child("parts.mtl"),
            "newmtl Red\nKd 1 0 0\n"
            "newmtl Blue\nKd 0 0 1\n"
            "newmtl AlsoRed\nKd 1 0 0\n");
  WriteText(path,
            "mtllib parts.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\n"
            "o a\nusemtl Red\nf 1//1 2//1 3//1\n"
            "o b\nusemtl Blue\nf 1//1 2//1 3//1\n"
            "o c\nusemtl AlsoRed\nf 1//1 2//1 3//1\n"
            "o d\nusemtl Blue\nf 1//1 2//1 3//1\n");

  const auto result = ImportModel(path);

  REQUIRE(result.ok());
  const Model& model = result.value();
  // Red and AlsoRed look the same and share one entry.
  REQUIRE(model.materials.size() == 2);
  REQUIRE(model.mesh.submeshes.size() == 2);
  testing::CheckSubmeshes(model.mesh, model.materials.size());
  CHECK(FindMaterial(model, "Red") != nullptr);
  CHECK(FindMaterial(model, "Blue") != nullptr);
  CHECK(model.mesh.submeshes[0].material != model.mesh.submeshes[1].material);
  for (const Submesh& submesh : model.mesh.submeshes) {
    // Two triangles per material.
    CHECK(submesh.index_count == 6);
  }
}

TEST_CASE("A sidecar .bmat file overrides an imported material") {
  const testing::ScratchDirectory dir("sidecar_model");
  const std::string path = dir.Child("box.obj");
  WriteText(dir.Child("box.mtl"), "newmtl Body Paint\nKd 0.2 0.2 0.2\n");
  WriteText(path,
            "mtllib box.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 0 1 0\nvn 0 0 1\n"
            "usemtl Body Paint\nf 1//1 2//1 3//1\n");
  // Spaces in material names become underscores in the sidecar name.
  const std::string sidecar = dir.Child("box.obj.Body_Paint.bmat");
  CHECK(material::SidecarMaterialPath(path, "Body Paint") == sidecar);
  WriteText(sidecar, "name = chrome\nmetallic = 1\nroughness = 0.1\n");

  const auto overridden = ImportModel(path);
  const auto raw = ImportModel(path, {.use_material_overrides = false});

  REQUIRE(overridden.ok());
  REQUIRE(overridden.value().materials.size() == 1);
  CHECK(overridden.value().materials[0].name == "chrome");
  CHECK(overridden.value().materials[0].metallic == 1.0f);
  CHECK(overridden.value().materials[0].roughness == doctest::Approx(0.1f));
  REQUIRE(raw.ok());
  REQUIRE(raw.value().materials.size() == 1);
  CHECK(raw.value().materials[0].name == "Body Paint");
  CHECK(raw.value().materials[0].metallic == 0.0f);
}

TEST_CASE("ImportModel reads the M16 materials from its MTL file") {
  const auto result = ImportModel(core::JoinPath(BLUE_SAMPLES_DIR, "m16.obj"),
                                  {.use_material_overrides = false});
  REQUIRE(result.ok());
  const Model& model = result.value();
  testing::CheckSubmeshes(model.mesh, model.materials.size());
  testing::CheckTangentFrames(model.mesh, /*min_agreement=*/0.0f);

  // Five MTL materials; Grip and Stock are identical (black polymer), so
  // the table holds four.
  CHECK(model.materials.size() == 4);
  struct Expected {
    std::string_view name;
    glm::vec3 color;
  };
  for (const Expected& expected :
       {Expected{"BaseMetal", {0.105f, 0.105f, 0.11f}},
        Expected{"Clip", {0.18f, 0.175f, 0.165f}},
        Expected{"Grip", {0.06f, 0.06f, 0.062f}},
        Expected{"Silencer", {0.075f, 0.073f, 0.07f}}}) {
    const std::string name(expected.name);
    CAPTURE(name);
    const material::MaterialDesc* found = FindMaterial(model, expected.name);
    REQUIRE(found != nullptr);
    CHECK(NearlyEqual(glm::vec3(found->base_color), expected.color));
    CHECK(found->metallic == 0.0f);
    CHECK(found->roughness == doctest::Approx(0.5f));
    CHECK(found->base_color_texture.empty());
  }
}

}  // namespace blue::asset
