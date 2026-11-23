#include "cook/prefab_split.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_geometric.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "asset/mesh_data.h"
#include "cook/model_cook.h"
#include "cook/prefab_edits.h"
#include "core/result.h"
#include "platform/file_system.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "tests/prefab_samples.h"
#include "tests/scratch_directory.h"

namespace blue::cook {
namespace {

bool Near(glm::vec3 a, glm::vec3 b, float tolerance = 1.0e-5f) {
  return glm::length(a - b) <= tolerance;
}

// A weapon-like prefab: root "gun" (scale 0.5) > "frame" > "slide",
// "barrel"; "mag" under the root. One mesh per node except the root;
// frame uses material "polymer" (texture 0), slide and barrel "steel"
// (texture 1), mag "mag" (texture 2). A clip moves the slide and the root.
prefab::Prefab MakeGun() {
  prefab::Prefab gun;
  gun.hierarchy.nodes = {
      {.name = "gun", .parent = -1, .rest = {.scale = glm::vec3(0.5f)}},
      {.name = "frame", .parent = 0, .rest = {.translation = {1, 0, 0}}},
      {.name = "slide", .parent = 1, .rest = {.translation = {0, 1, 0}}},
      {.name = "barrel", .parent = 1, .rest = {.translation = {0, 1, -2}}},
      {.name = "mag", .parent = 0, .rest = {.translation = {2, -1, 0}}},
  };
  for (uint32_t i = 0; i < 3; ++i) {
    const std::string digit(1, static_cast<char>('0' + i));
    gun.textures.push_back(testing::MakePatternTexture(
        gun, "texture" + digit, prefab::TextureFormat::kRgba8Unorm, 4, 4, i));
    gun.edit.texture_sources.push_back({"src" + digit + ".png"});
  }
  gun.materials.push_back({.name = "polymer", .textures = {0, -1, -1, -1}});
  gun.materials.push_back({.name = "steel", .textures = {1, -1, -1, -1}});
  gun.materials.push_back({.name = "mag", .textures = {-1, -1, 2, -1}});
  const asset::MeshData cube = asset::MakeCube();
  const auto add_mesh = [&](const char* name, int node, int material) {
    prefab::Mesh mesh{.name = name, .node = node, .bounds = cube.bounds};
    mesh.vertices =
        gun.storage.StoreArray<asset::Vertex>(std::span(cube.vertices));
    mesh.indices =
        gun.storage.StoreArray<asset::Index>(std::span(cube.indices));
    mesh.submeshes = {
        {.index_count = static_cast<uint32_t>(cube.indices.size()),
         .material = material}};
    gun.meshes.push_back(std::move(mesh));
  };
  add_mesh("frame", 1, 0);
  add_mesh("slide", 2, 1);
  add_mesh("barrel", 3, 1);
  add_mesh("mag", 4, 2);

  animation::Clip fire{.name = "fire", .duration = 1.0f};
  fire.channels.push_back({.node = 2,
                           .path = animation::TargetPath::kTranslation,
                           .times = {0.0f, 1.0f},
                           .values = {0, 1, 0, 0, 1, 1}});
  fire.channels.push_back({.node = 0,
                           .path = animation::TargetPath::kTranslation,
                           .times = {0.0f, 1.0f},
                           .values = {0, 0, 0, 1, 0, 0}});
  animation::AddEvent(fire, {.name = "shot", .time = 0.25f});
  gun.clips.push_back(fire);
  gun.sockets.push_back(
      {.name = "sight", .node = 2, .local = {.translation = {0, 0.5f, 0}}});
  gun.sockets.push_back(
      {.name = "grip", .node = -1, .local = {.translation = {1, -1, 0}}});
  gun.tags = {{.key = "weapon", .value = "gun"}};
  gun.physics.kind = prefab::ColliderKind::kBox;
  gun.edit.source_model = "gun.glb";
  gun.edit.import_settings = "scale = 0.5\n";
  gun.bounds = ComputePrefabBounds(gun);
  return gun;
}

prefab::Prefab Split(const prefab::Prefab& source,
                     const SplitOptions& options) {
  core::Result<prefab::Prefab, std::string> part = SplitPrefab(source, options);
  if (!part.ok()) {
    FAIL(part.error());
  }
  CHECK(prefab::ValidatePrefab(part.value()) == std::nullopt);
  return std::move(part).value();
}

std::string SplitError(const prefab::Prefab& source,
                       const SplitOptions& options) {
  core::Result<prefab::Prefab, std::string> part = SplitPrefab(source, options);
  REQUIRE(!part.ok());
  return part.error();
}

// Position of vertex 0 of mesh `mesh` at the rest pose, in prefab space.
glm::vec3 FirstVertex(const prefab::Prefab& prefab, size_t mesh) {
  const prefab::Mesh& m = prefab.meshes[mesh];
  const glm::mat4 world =
      RestWorldMatrices(prefab)[static_cast<size_t>(m.node)];
  return glm::vec3(world * glm::vec4(m.vertices[0].position, 1.0f));
}

std::vector<std::byte> Bytes(std::span<const std::byte> bytes) {
  return {bytes.begin(), bytes.end()};
}

TEST_CASE("Splitting a node keeps its mesh, materials and sockets only") {
  const prefab::Prefab gun = MakeGun();
  const prefab::Prefab part = Split(gun, {.nodes = {"slide"}});
  REQUIRE(part.hierarchy.nodes.size() == 1);
  const animation::Node& root = part.hierarchy.nodes[0];
  CHECK(root.name == "slide");
  CHECK(root.parent == -1);
  // Origin: the slide's rest position (0.5, 0.5, 0); the root's scale stays.
  CHECK(Near(root.rest.translation, glm::vec3(0.0f)));
  CHECK(Near(root.rest.scale, glm::vec3(0.5f)));
  CHECK(Near(FirstVertex(part, 0) + glm::vec3(0.5f, 0.5f, 0.0f),
             FirstVertex(gun, 1)));

  REQUIRE(part.meshes.size() == 1);
  CHECK(part.meshes[0].name == "slide");
  CHECK(part.meshes[0].node == 0);
  REQUIRE(part.materials.size() == 1);
  CHECK(part.materials[0].name == "steel");
  CHECK(part.meshes[0].submeshes[0].material == 0);
  REQUIRE(part.textures.size() == 1);
  CHECK(part.textures[0].name == "texture1");
  CHECK(part.materials[0].texture(prefab::TextureSlot::kBaseColor) == 0);
  REQUIRE(part.edit.texture_sources.size() == 1);
  CHECK(part.edit.texture_sources[0] == std::vector<std::string>{"src1.png"});

  REQUIRE(part.sockets.size() == 1);  // grip (root) dropped
  CHECK(part.sockets[0].name == "sight");
  CHECK(part.sockets[0].node == 0);
  CHECK(part.clips.empty());
  CHECK(part.tags.empty());
  CHECK(part.physics.kind == prefab::ColliderKind::kNone);
  CHECK(part.edit.source_model.empty());
  CHECK(part.edit.import_settings.empty());
  CHECK(Near(part.bounds.size(), glm::vec3(0.5f)));
  // Blocks no kept array views are dropped.
  CHECK(part.storage.byte_size() < gun.storage.byte_size());
}

TEST_CASE("Descendants come along unless excluded") {
  const prefab::Prefab gun = MakeGun();
  const prefab::Prefab all = Split(gun, {.nodes = {"frame"}});
  CHECK(all.meshes.size() == 3);
  CHECK(all.materials.size() == 2);
  CHECK(all.textures.size() == 2);

  const prefab::Prefab part =
      Split(gun, {.nodes = {"frame"}, .exclude = {"barrel"}});
  REQUIRE(part.hierarchy.nodes.size() == 2);
  CHECK(part.hierarchy.nodes[0].name == "frame");
  CHECK(part.hierarchy.nodes[1].name == "slide");
  CHECK(part.hierarchy.nodes[1].parent == 0);
  REQUIRE(part.meshes.size() == 2);
  CHECK(part.meshes[0].name == "frame");
  CHECK(part.meshes[1].name == "slide");
  CHECK(part.materials.size() == 2);
  // The slide keeps its place relative to the frame (the origin: the frame,
  // at (0.5, 0, 0)).
  CHECK(Near(FirstVertex(part, 1) + glm::vec3(0.5f, 0.0f, 0.0f),
             FirstVertex(gun, 1)));
}

TEST_CASE("The origin and the forward axis set the part frame") {
  const prefab::Prefab gun = MakeGun();
  // The barrel sits at (0.5, 0.5, -1); forward -x is turned onto +z.
  const prefab::Prefab part = Split(gun, {.nodes = {"barrel"},
                                          .origin_point = glm::vec3(0.0f),
                                          .forward = {-1.0f, 0.0f, 0.0f}});
  const animation::Transform& rest = part.hierarchy.nodes[0].rest;
  CHECK(Near(rest.translation, glm::vec3(-1.0f, 0.5f, -0.5f)));
  CHECK(Near(rest.rotation * glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1)));
  CHECK(Near(rest.rotation * glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)));

  const prefab::Prefab by_node =
      Split(gun, {.nodes = {"barrel"}, .origin_node = "frame"});
  CHECK(Near(by_node.hierarchy.nodes[0].rest.translation,
             glm::vec3(0.0f, 0.5f, -1.0f)));
}

TEST_CASE("--keep-skeleton keeps nodes, clips and events, chosen meshes") {
  const prefab::Prefab gun = MakeGun();
  const prefab::Prefab part = Split(gun, {.nodes = {"frame"},
                                          .exclude = {"slide", "barrel"},
                                          .forward = {-1.0f, 0.0f, 0.0f},
                                          .keep_skeleton = true});
  REQUIRE(part.hierarchy.nodes.size() == gun.hierarchy.nodes.size());
  REQUIRE(part.meshes.size() == 1);
  CHECK(part.meshes[0].name == "frame");
  CHECK(part.meshes[0].node == 1);
  REQUIRE(part.materials.size() == 1);
  CHECK(part.materials[0].name == "polymer");
  CHECK(part.textures.size() == 1);

  // The root is turned; children keep their local rest transforms.
  const glm::quat turn = part.hierarchy.nodes[0].rest.rotation;
  CHECK(Near(turn * glm::vec3(-1, 0, 0), glm::vec3(0, 0, 1)));
  CHECK(Near(part.hierarchy.nodes[2].rest.translation, glm::vec3(0, 1, 0)));

  REQUIRE(part.clips.size() == 1);
  const animation::Clip& fire = part.clips[0];
  CHECK(animation::FindEvent(fire, "shot") != nullptr);
  // The slide's channel is untouched; the root's translation is turned.
  CHECK(fire.channels[0].values == gun.clips[0].channels[0].values);
  const std::vector<float>& root_keys = fire.channels[1].values;
  REQUIRE(root_keys.size() == 6);
  CHECK(Near({root_keys[3], root_keys[4], root_keys[5]}, {0, 0, -1}));

  // Both sockets stay; the root one is re-expressed.
  REQUIRE(part.sockets.size() == 2);
  CHECK(part.sockets[0].node == 2);
  CHECK(Near(part.sockets[1].local.translation, glm::vec3(0, -1, -1)));
  CHECK(part.tags.empty());
  CHECK(part.physics.kind == prefab::ColliderKind::kNone);
}

TEST_CASE("Bad splits fail with a message") {
  const prefab::Prefab gun = MakeGun();
  CHECK(!SplitError(gun, {}).empty());
  CHECK(SplitError(gun, {.nodes = {"stock"}}).find("stock") !=
        std::string::npos);
  CHECK(SplitError(gun, {.nodes = {"slide"}, .exclude = {"x"}}).find("'x'") !=
        std::string::npos);
  CHECK(SplitError(gun, {.nodes = {"slide"}, .origin_node = "y"}).find("'y'") !=
        std::string::npos);
  CHECK(SplitError(gun, {.nodes = {"slide"}, .exclude = {"slide"}})
            .find("no meshes") != std::string::npos);
  CHECK(SplitError(gun, {.nodes = {"slide"},
                         .origin_node = "frame",
                         .origin_point = glm::vec3(0.0f)})
            .find("not both") != std::string::npos);
}

TEST_CASE("Split options parse") {
  SplitOptions options;
  CHECK(ParseSplitOption(options, "--nodes", "Frame,Slide").ok());
  CHECK(ParseSplitOption(options, "--nodes", "Barrel").ok());
  CHECK(options.nodes == std::vector<std::string>{"Frame", "Slide", "Barrel"});
  CHECK(ParseSplitOption(options, "--exclude", "Slide").ok());
  CHECK(options.exclude == std::vector<std::string>{"Slide"});
  CHECK(ParseSplitOption(options, "--origin", "Slide").ok());
  CHECK(options.origin_node == "Slide");
  CHECK(ParseSplitOption(options, "--origin", "0,0.1263,-0.092").ok());
  CHECK(!options.origin_node);
  REQUIRE(options.origin_point);
  CHECK(Near(*options.origin_point, {0.0f, 0.1263f, -0.092f}));
  CHECK(ParseSplitOption(options, "--forward", "-x").ok());
  CHECK(Near(options.forward, {-1, 0, 0}));
  CHECK(ApplySplitFlag(options, "--keep-skeleton"));
  CHECK(options.keep_skeleton);
  CHECK(!ApplySplitFlag(options, "--nodes"));
  CHECK(IsSplitOption("--origin"));
  CHECK(!IsSplitOption("--tag"));

  CHECK(!ParseSplitOption(options, "--nodes", ",").ok());
  CHECK(!ParseSplitOption(options, "--origin", "1,2").ok());
  CHECK(!ParseSplitOption(options, "--origin", "1,2,x").ok());
  CHECK(!ParseSplitOption(options, "--forward", "+y").ok());
  CHECK(!ParseSplitOption(options, "--bogus", "1").ok());
}

TEST_CASE("SocketAtPrefabFrame compensates the node's transform") {
  const prefab::Prefab gun = MakeGun();
  const animation::Transform frame{
      .translation = {0.1f, 0.2f, 0.3f},
      .rotation = glm::angleAxis(0.5f, glm::vec3(0, 1, 0))};
  core::Result<prefab::Socket, std::string> socket =
      SocketAtPrefabFrame(gun, "slot.slide", 2, frame);
  REQUIRE(socket.ok());
  CHECK(socket.value().node == 2);
  const glm::mat4 world =
      RestWorldMatrices(gun)[2] * animation::ToMatrix(socket.value().local);
  const glm::mat4 expected = animation::ToMatrix(frame);
  for (int column = 0; column < 4; ++column) {
    CHECK(Near(glm::vec3(world[column]), glm::vec3(expected[column])));
  }
  CHECK(!SocketAtPrefabFrame(gun, "x", 9, frame).ok());
}

TEST_CASE("DecomposeMatrix rejects shear and mirrors") {
  const animation::Transform transform{
      .translation = {1, 2, 3},
      .rotation = glm::angleAxis(0.3f, glm::normalize(glm::vec3(1, 1, 0))),
      .scale = {2, 3, 4}};
  const std::optional<animation::Transform> back =
      DecomposeMatrix(animation::ToMatrix(transform));
  REQUIRE(back);
  CHECK(Near(back->translation, transform.translation));
  CHECK(Near(back->scale, transform.scale, 1.0e-4f));
  CHECK(std::abs(glm::dot(back->rotation, transform.rotation)) ==
        doctest::Approx(1.0f));
  glm::mat4 shear(1.0f);
  shear[1][0] = 0.5f;
  CHECK(!DecomposeMatrix(shear));
  glm::mat4 mirror(1.0f);
  mirror[0][0] = -1.0f;
  CHECK(!DecomposeMatrix(mirror));
}

TEST_CASE("SplitPrefabFile writes the part and never touches the input") {
  testing::ScratchDirectory scratch("prefab_split");
  const std::string input = scratch.Child("gun.pbo");
  const std::string output = scratch.Child("parts/slide.pbo");
  const prefab::Prefab gun = MakeGun();
  REQUIRE(prefab::SavePrefab(gun, input).ok());
  auto before = platform::ReadFile(input, prefab::kMaxPrefabFileSize);
  REQUIRE(before.ok());

  const SplitOptions options{.nodes = {"slide"}, .forward = {-1, 0, 0}};
  const std::vector<PrefabEdit> edits{
      ParsePrefabEdit("--tag", "part=gun_slide").value(),
      ParsePrefabEdit("--socket", "slot.sight=-@0,0.25,0").value()};
  REQUIRE(SplitPrefabFile(input, output, options, edits).ok());
  auto after = platform::ReadFile(input, prefab::kMaxPrefabFileSize);
  REQUIRE(after.ok());
  CHECK(after.value() == before.value());

  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(output);
  REQUIRE(loaded.ok());
  const prefab::Prefab& part = loaded.value();
  const prefab::Prefab expected = Split(gun, options);
  CHECK(prefab::FindTag(part, "part") == "gun_slide");
  CHECK(prefab::FindSocket(part, "slot.sight") >= 0);
  CHECK(prefab::FindSocket(part, "sight") >= 0);
  REQUIRE(part.meshes.size() == expected.meshes.size());
  CHECK(Bytes(std::as_bytes(part.meshes[0].vertices)) ==
        Bytes(std::as_bytes(expected.meshes[0].vertices)));
  CHECK(part.materials.size() == expected.materials.size());
  REQUIRE(part.textures.size() == 1);
  REQUIRE(part.textures[0].mips.size() == expected.textures[0].mips.size());
  for (size_t i = 0; i < part.textures[0].mips.size(); ++i) {
    CHECK(Bytes(part.textures[0].mips[i].data) ==
          Bytes(expected.textures[0].mips[i].data));
  }
  CHECK(Near(part.bounds.min, expected.bounds.min));
  CHECK(Near(part.bounds.max, expected.bounds.max));
  CHECK(Near(part.hierarchy.nodes[0].rest.translation,
             expected.hierarchy.nodes[0].rest.translation));

  // The input is refused as the output; a failed split writes nothing.
  CHECK(!SplitPrefabFile(input, input, options, {}).ok());
  const std::string missing = scratch.Child("parts/none.pbo");
  CHECK(!SplitPrefabFile(input, missing, {.nodes = {"stock"}}, {}).ok());
  CHECK(!platform::ReadFile(missing, 1024).ok());
}

}  // namespace
}  // namespace blue::cook
