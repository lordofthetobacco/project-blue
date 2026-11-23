// Cooks the real sample models into prefabs and checks what the game and
// the editor will rely on. Textures are cooked small and fast to keep the
// suite quick; full-size cook timings are logged by blue-pack.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/vec3.hpp>

#include "animation/hierarchy.h"
#include "asset/animated_model.h"
#include "asset/model_importer.h"
#include "cook/model_cook.h"
#include "core/path.h"
#include "material/material.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/scratch_directory.h"

namespace blue::cook {
namespace {

std::string Sample(const char* name) {
  return core::JoinPath(BLUE_SAMPLES_DIR, name);
}

// Options that keep test cooks quick: small textures, fast compression.
ModelCookOptions QuickOptions() {
  return {.texture_quality = CookQuality::kFast, .max_texture_size = 128};
}

size_t TriangleCount(const prefab::Prefab& prefab) {
  size_t count = 0;
  for (const prefab::Mesh& mesh : prefab.meshes) {
    count += mesh.indices.size() / 3;
  }
  return count;
}

// Checks the invariants every cooked mesh must hold.
void CheckMeshes(const prefab::Prefab& prefab) {
  for (const prefab::Mesh& mesh : prefab.meshes) {
    CAPTURE(mesh.name);
    REQUIRE(!mesh.vertices.empty());
    CHECK(mesh.indices.size() % 3 == 0);
    CHECK(std::ranges::all_of(mesh.indices, [&](asset::Index index) {
      return index < mesh.vertices.size();
    }));
    uint32_t covered = 0;
    for (const asset::Submesh& submesh : mesh.submeshes) {
      CHECK(submesh.first_index == covered);
      covered += submesh.index_count;
      CHECK(submesh.material >= 0);
      CHECK(static_cast<size_t>(submesh.material) < prefab.materials.size());
    }
    CHECK(covered == mesh.indices.size());
  }
}

TEST_CASE("The Glock cooks into a prefab with BC textures and its clips") {
  ModelCookOptions options = QuickOptions();
  options.drop_nodes = {"Glock_parts_ORIGINAL_combined"};
  auto cooked = CookModel(Sample("glock/glock17.glb"), options);
  REQUIRE(cooked.ok());
  const prefab::Prefab& prefab = cooked.value();
  CHECK(prefab::ValidatePrefab(prefab) == std::nullopt);

  CHECK(animation::FindNode(prefab.hierarchy, "Glock_parts_ORIGINAL_combined") <
        0);
  CHECK(animation::FindNode(prefab.hierarchy, "Slide") >= 0);
  CHECK(animation::FindNode(prefab.hierarchy, "Trigger") >= 0);
  CHECK(prefab.meshes.size() == 6);
  CheckMeshes(prefab);

  REQUIRE(prefab.materials.size() == 2);
  REQUIRE(prefab.textures.size() == 6);
  for (const prefab::Material& material : prefab.materials) {
    CAPTURE(material.name);
    CHECK(material.texture(prefab::TextureSlot::kBaseColor) >= 0);
    CHECK(material.texture(prefab::TextureSlot::kOrm) >= 0);
    CHECK(material.texture(prefab::TextureSlot::kNormal) >= 0);
    CHECK(prefab
              .textures[static_cast<size_t>(
                  material.texture(prefab::TextureSlot::kBaseColor))]
              .format == prefab::TextureFormat::kBc7Srgb);
    CHECK(prefab
              .textures[static_cast<size_t>(
                  material.texture(prefab::TextureSlot::kOrm))]
              .format == prefab::TextureFormat::kBc7Unorm);
    CHECK(prefab
              .textures[static_cast<size_t>(
                  material.texture(prefab::TextureSlot::kNormal))]
              .format == prefab::TextureFormat::kBc5Unorm);
  }
  for (const prefab::Texture& texture : prefab.textures) {
    CAPTURE(texture.name);
    CHECK(texture.width == 128);
    CHECK(texture.mip_count() ==
          prefab::FullMipCount(texture.width, texture.height));
  }
  REQUIRE(prefab.edit.texture_sources.size() == prefab.textures.size());
  CHECK(prefab.edit.source_model == Sample("glock/glock17.glb"));

  REQUIRE(prefab.clips.size() == 4);
  for (const animation::Clip& clip : prefab.clips) {
    CHECK(clip.duration > 0.0f);
    CHECK(!clip.loop);
  }

  // The importer's view of the file: same geometry after optimisation.
  auto imported = asset::ImportAnimatedModel(Sample("glock/glock17.glb"));
  REQUIRE(imported.ok());
  size_t imported_triangles = 0;
  for (const asset::ModelPart& part : imported.value().parts) {
    if (imported.value().hierarchy.nodes[static_cast<size_t>(part.node)].name !=
        "Glock_parts_ORIGINAL_combined") {
      imported_triangles += part.mesh.indices.size() / 3;
    }
  }
  CHECK(TriangleCount(prefab) == imported_triangles);
}

TEST_CASE("A cooked Glock with sockets survives a save and load") {
  ModelCookOptions options = QuickOptions();
  options.max_texture_size = 16;
  options.drop_nodes = {"Glock_parts_ORIGINAL_combined"};
  auto cooked = CookModel(Sample("glock/glock17.glb"), options);
  REQUIRE(cooked.ok());
  prefab::Prefab prefab = std::move(cooked).value();
  const int slide = animation::FindNode(prefab.hierarchy, "Slide");
  REQUIRE(slide >= 0);
  prefab.sockets.push_back(
      {.name = "muzzle", .node = slide, .local = {.translation = {1, 2, 3}}});

  testing::ScratchDirectory scratch("cook_model");
  const std::string path = scratch.Child("glock.pbo");
  REQUIRE(prefab::SavePrefab(prefab, path).ok());
  auto loaded = prefab::LoadPrefab(path);
  REQUIRE(loaded.ok());
  const prefab::Prefab& reloaded = loaded.value();
  REQUIRE(reloaded.sockets.size() == 1);
  CHECK(reloaded.sockets[0].name == "muzzle");
  CHECK(reloaded.sockets[0].node == slide);
  CHECK(reloaded.sockets[0].local.translation.z == 3.0f);
  CHECK(reloaded.textures.size() == prefab.textures.size());
  CHECK(reloaded.clips.size() == 4);
  CHECK(reloaded.edit.source_model == prefab.edit.source_model);

  auto settings = ParseModelCookOptions(reloaded.edit.import_settings);
  REQUIRE(settings.ok());
  CHECK(settings.value().drop_nodes == options.drop_nodes);
  CHECK(settings.value().max_texture_size == 16);
}

TEST_CASE("The M16 cooks merged into one mesh with its sidecar materials") {
  ModelCookOptions options = QuickOptions();
  options.merge_static_nodes = true;
  auto cooked = CookModel(Sample("m16.obj"), options);
  REQUIRE(cooked.ok());
  const prefab::Prefab& prefab = cooked.value();
  CHECK(prefab::ValidatePrefab(prefab) == std::nullopt);
  CHECK(prefab.hierarchy.nodes.empty());
  REQUIRE(prefab.meshes.size() == 1);
  CHECK(prefab.meshes[0].node == -1);
  CheckMeshes(prefab);

  auto imported = asset::ImportModel(Sample("m16.obj"));
  REQUIRE(imported.ok());
  const asset::MeshData& reference = imported.value().mesh;
  CHECK(prefab.materials.size() == imported.value().materials.size());
  CHECK(prefab.meshes[0].submeshes.size() == reference.submeshes.size());
  CHECK(TriangleCount(prefab) == reference.indices.size() / 3);
  CHECK(prefab.meshes[0].vertices.size() <= reference.vertices.size());
  for (int axis = 0; axis < 3; ++axis) {
    CHECK(prefab.bounds.min[axis] ==
          doctest::Approx(reference.bounds.min[axis]).epsilon(1e-4));
    CHECK(prefab.bounds.max[axis] ==
          doctest::Approx(reference.bounds.max[axis]).epsilon(1e-4));
  }
}

TEST_CASE("The house's many parts merge into a single static mesh") {
  ModelCookOptions options = QuickOptions();
  options.merge_static_nodes = true;
  auto cooked = CookModel(Sample("house.obj"), options);
  REQUIRE(cooked.ok());
  const prefab::Prefab& prefab = cooked.value();
  CHECK(prefab::ValidatePrefab(prefab) == std::nullopt);
  CHECK(prefab.hierarchy.nodes.empty());
  REQUIRE(prefab.meshes.size() == 1);
  CheckMeshes(prefab);
  auto imported = asset::ImportModel(Sample("house.obj"));
  REQUIRE(imported.ok());
  CHECK(TriangleCount(prefab) == imported.value().mesh.indices.size() / 3);
  CHECK(prefab.meshes[0].submeshes.size() ==
        imported.value().mesh.submeshes.size());
  CHECK(prefab.bounds.min.y ==
        doctest::Approx(imported.value().mesh.bounds.min.y).epsilon(1e-4));
}

TEST_CASE("Scale and up-axis correction add a root or bake into the mesh") {
  auto plain = CookModel(Sample("teapot.obj"), QuickOptions());
  REQUIRE(plain.ok());
  const asset::Bounds original = plain.value().bounds;

  ModelCookOptions corrected = QuickOptions();
  corrected.scale = 2.0f;
  corrected.up_axis = UpAxis::kZ;
  auto rooted = CookModel(Sample("teapot.obj"), corrected);
  REQUIRE(rooted.ok());
  const prefab::Prefab& prefab = rooted.value();
  REQUIRE(!prefab.hierarchy.nodes.empty());
  CHECK(prefab.hierarchy.nodes[0].name == "teapot");
  CHECK(prefab.hierarchy.nodes[0].parent == -1);
  for (size_t i = 1; i < prefab.hierarchy.nodes.size(); ++i) {
    CHECK(prefab.hierarchy.nodes[i].parent >= 0);
  }
  // Z-up: the source's z extent becomes the y extent, doubled.
  CHECK(prefab.bounds.size().y ==
        doctest::Approx(2.0f * original.size().z).epsilon(1e-3));
  CHECK(prefab.bounds.size().x ==
        doctest::Approx(2.0f * original.size().x).epsilon(1e-3));

  corrected.merge_static_nodes = true;
  auto baked = CookModel(Sample("teapot.obj"), corrected);
  REQUIRE(baked.ok());
  CHECK(baked.value().hierarchy.nodes.empty());
  CHECK(baked.value().bounds.size().y ==
        doctest::Approx(2.0f * original.size().z).epsilon(1e-3));
}

TEST_CASE("CookModel reports missing files and bad options") {
  CHECK(CookModel(Sample("missing.glb")).error() == CookError::kFileNotFound);
  CHECK(CookModel(Sample("teapot.obj"), {.scale = 0.0f}).error() ==
        CookError::kInvalidArgument);
}

TEST_CASE("AddMaterialToPrefab cooks embedded textures, or changes nothing") {
  prefab::Prefab prefab;
  const ModelCookOptions options = QuickOptions();
  material::MaterialDesc desc{.name = "glock"};
  desc.base_color_texture = {.path = Sample("glock/glock17.glb"),
                             .embedded_index = 0};
  desc.metallic_roughness_texture = desc.base_color_texture;
  auto added = AddMaterialToPrefab(prefab, desc, options);
  REQUIRE(added.ok());
  CHECK(added.value() == 0);
  REQUIRE(prefab.textures.size() == 2);  // base colour + ORM from one image
  CHECK(prefab.textures[0].format == prefab::TextureFormat::kBc7Srgb);
  CHECK(prefab.textures[1].format == prefab::TextureFormat::kBc7Unorm);
  CHECK(prefab.textures[0].name == "glock17_0");
  CHECK(prefab.textures[1].name == "glock_orm");
  CHECK(!prefab.textures[1].mips.front().data.empty());

  // A missing image fails without leaving reserved texture slots behind.
  desc.normal_texture = {.path = Sample("missing.png")};
  CHECK(AddMaterialToPrefab(prefab, desc, options).error() ==
        CookError::kTextureLoadFailed);
  CHECK(prefab.materials.size() == 1);
  CHECK(prefab.textures.size() == 2);
  CHECK(prefab.edit.texture_sources.size() == 2);
}

TEST_CASE("Cook options round-trip through their text form") {
  const ModelCookOptions options{.scale = 0.01f,
                                 .up_axis = UpAxis::kZ,
                                 .merge_static_nodes = true,
                                 .drop_nodes = {"a", "b c"},
                                 .use_material_overrides = false,
                                 .optimize_meshes = false,
                                 .texture_quality = CookQuality::kFast,
                                 .compress_textures = false,
                                 .max_texture_size = 512};
  auto parsed = ParseModelCookOptions(FormatModelCookOptions(options));
  REQUIRE(parsed.ok());
  const ModelCookOptions& back = parsed.value();
  CHECK(back.scale == options.scale);
  CHECK(back.up_axis == UpAxis::kZ);
  CHECK(back.merge_static_nodes);
  CHECK(back.drop_nodes == options.drop_nodes);
  CHECK(!back.use_material_overrides);
  CHECK(!back.optimize_meshes);
  CHECK(back.texture_quality == CookQuality::kFast);
  CHECK(!back.compress_textures);
  CHECK(back.max_texture_size == 512);

  CHECK(ParseModelCookOptions("future_key = 3\nscale = 2\n").ok());
  CHECK(ParseModelCookOptions("scale = -1\n").error() ==
        CookError::kInvalidArgument);
  CHECK(ParseModelCookOptions("no equals sign\n").error() ==
        CookError::kInvalidArgument);
}

}  // namespace
}  // namespace blue::cook
