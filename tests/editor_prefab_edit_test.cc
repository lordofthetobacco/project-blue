#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "cook/cook_error.h"
#include "editor/prefab_mode/prefab_edit.h"
#include "prefab/prefab.h"
#include "prefab_samples.h"

namespace blue::editor {
namespace {

using prefab::Prefab;

bool NearlyEqual(const glm::mat4& a, const glm::mat4& b, float eps = 1e-4f) {
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      if (std::abs(a[c][r] - b[c][r]) > eps) {
        return false;
      }
    }
  }
  return true;
}

// The sample prefab plus a third node "stock" under the root.
Prefab ThreeNodePrefab() {
  Prefab prefab = testing::MakeSamplePrefab();
  prefab.hierarchy.nodes.push_back(
      {.name = "stock",
       .parent = 0,
       .rest = {.translation = {0.0f, -1.0f, 0.5f},
                .rotation = glm::angleAxis(glm::radians(30.0f),
                                           glm::vec3(0.0f, 1.0f, 0.0f))}});
  return prefab;
}

TEST_CASE("MakeUniqueName appends the first free number") {
  const std::vector<std::string> taken{"socket", "socket 2", "light"};
  CHECK(MakeUniqueName("muzzle", taken) == "muzzle");
  CHECK(MakeUniqueName("socket", taken) == "socket 3");
  CHECK(MakeUniqueName("light", taken) == "light 2");
}

TEST_CASE("DecomposeMatrix round-trips translation, rotation and scale") {
  const animation::Transform transform{
      .translation = {1.0f, -2.0f, 3.0f},
      .rotation = glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f)),
      .scale = {2.0f, 0.5f, 1.5f}};
  const animation::Transform back =
      DecomposeMatrix(animation::ToMatrix(transform));
  CHECK(NearlyEqual(animation::ToMatrix(back), animation::ToMatrix(transform)));
}

TEST_CASE("RemoveNode drops the subtree and everything attached to it") {
  Prefab prefab = ThreeNodePrefab();
  REQUIRE_FALSE(prefab::ValidatePrefab(prefab));
  REQUIRE(RemoveNode(prefab, 1));
  CHECK(prefab.hierarchy.nodes.size() == 2);
  CHECK(prefab.hierarchy.nodes[1].name == "stock");
  CHECK(prefab.hierarchy.nodes[1].parent == 0);
  CHECK(prefab.meshes.empty());
  CHECK(prefab.sockets.empty());
  CHECK(prefab.children.empty());
  CHECK(prefab.lights.size() == 1);  // on the root
  CHECK(prefab.clips[0].channels.empty());
  UpdatePrefabBounds(prefab);
  CHECK_FALSE(prefab::ValidatePrefab(prefab));

  CHECK_FALSE(RemoveNode(prefab, 5));
  REQUIRE(RemoveNode(prefab, 0));
  CHECK(prefab.hierarchy.nodes.empty());
  CHECK(prefab.lights.empty());
}

TEST_CASE("ReparentNode keeps the model-space pose and parents-first order") {
  Prefab prefab = ThreeNodePrefab();
  const glm::mat4 slide_world = NodeWorldMatrix(prefab, 1);

  // "slide" (1) moves under "stock" (2): stock must now come before it.
  REQUIRE(ReparentNode(prefab, 1, 2));
  CHECK(animation::IsValid(prefab.hierarchy));
  const int slide = animation::FindNode(prefab.hierarchy, "slide");
  const int stock = animation::FindNode(prefab.hierarchy, "stock");
  CHECK(stock < slide);
  CHECK(prefab.hierarchy.nodes[static_cast<size_t>(slide)].parent == stock);
  CHECK(NearlyEqual(NodeWorldMatrix(prefab, slide), slide_world));
  // Attachments followed the node.
  CHECK(prefab.meshes[0].node == slide);
  CHECK(prefab.sockets[0].node == slide);
  CHECK(prefab.clips[0].channels[0].node == slide);
  CHECK_FALSE(prefab::ValidatePrefab(prefab));

  // A node cannot move into its own subtree.
  CHECK_FALSE(ReparentNode(prefab, stock, slide));
  CHECK_FALSE(ReparentNode(prefab, stock, stock));

  // To a root.
  REQUIRE(ReparentNode(prefab, slide, -1));
  const int moved = animation::FindNode(prefab.hierarchy, "slide");
  CHECK(prefab.hierarchy.nodes[static_cast<size_t>(moved)].parent == -1);
  CHECK(NearlyEqual(NodeWorldMatrix(prefab, moved), slide_world));
}

TEST_CASE("SetSubmeshMaterial checks its indices") {
  Prefab prefab = testing::MakeSamplePrefab();
  CHECK(SetSubmeshMaterial(prefab, 0, 1, 0));
  CHECK(prefab.meshes[0].submeshes[1].material == 0);
  CHECK_FALSE(SetSubmeshMaterial(prefab, 0, 2, 0));
  CHECK_FALSE(SetSubmeshMaterial(prefab, 0, 0, 2));
  CHECK_FALSE(SetSubmeshMaterial(prefab, 1, 0, 0));
}

TEST_CASE("RemoveTexture clears slots and shifts later indices") {
  Prefab prefab = testing::MakeSamplePrefab();
  REQUIRE(RemoveTexture(prefab, 0));
  CHECK(prefab.textures.size() == 1);
  CHECK(prefab.materials[0].textures ==
        std::array<int, prefab::kTextureSlotCount>{-1, -1, 0, -1});
  REQUIRE(prefab.edit.texture_sources.size() == 1);
  CHECK(prefab.edit.texture_sources[0][0] == "sources/normal.png");
  CHECK_FALSE(prefab::ValidatePrefab(prefab));
  CHECK_FALSE(RemoveTexture(prefab, 1));
}

TEST_CASE("RemoveUnusedTextures keeps referenced textures") {
  Prefab prefab = testing::MakeSamplePrefab();
  prefab.textures.push_back(testing::MakePatternTexture(
      prefab, "orphan", prefab::TextureFormat::kRgba8Unorm, 4, 4, 3));
  CHECK(RemoveUnusedTextures(prefab) == 1);
  CHECK(prefab.textures.size() == 2);
  CHECK(RemoveUnusedTextures(prefab) == 0);
}

TEST_CASE("AssignTexture replaces an unshared texture and appends otherwise") {
  Prefab prefab = testing::MakeSamplePrefab();
  const prefab::Texture fresh = testing::MakePatternTexture(
      prefab, "", prefab::TextureFormat::kBc7Srgb, 4, 4, 7);

  // Slot 0 of "steel" is the only user of texture 0: replaced in place.
  CHECK(AssignTexture(prefab, 0, prefab::TextureSlot::kBaseColor, fresh,
                      {"new/albedo2.png"}) == 0);
  CHECK(prefab.textures.size() == 2);
  CHECK(prefab.textures[0].name == "albedo2");
  CHECK(prefab.edit.texture_sources[0][0] == "new/albedo2.png");

  // "grip" shares texture 0 now: its own assignment appends.
  prefab.materials[1].textures[0] = 0;
  CHECK(AssignTexture(prefab, 1, prefab::TextureSlot::kBaseColor, fresh,
                      {"new/grip.png"}) == 2);
  CHECK(prefab.materials[0].textures[0] == 0);
  CHECK(prefab.edit.texture_sources.size() == 3);
  CHECK_FALSE(prefab::ValidatePrefab(prefab));

  CHECK(AssignTexture(prefab, 9, prefab::TextureSlot::kBaseColor, fresh, {}) ==
        -1);
}

TEST_CASE("Slot texture imports reject bad arguments and missing files") {
  Prefab prefab = testing::MakeSamplePrefab();
  const auto missing = ImportSlotTexture(
      prefab, 0, prefab::TextureSlot::kNormal, "does/not/exist.png", {});
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error() == cook::CookError::kTextureLoadFailed);
  CHECK(ImportSlotTexture(prefab, 7, prefab::TextureSlot::kNormal, "x.png", {})
            .error() == cook::CookError::kInvalidArgument);
  CHECK(ImportOrmTexture(prefab, 0, {}, {}).error() ==
        cook::CookError::kInvalidArgument);
  OrmImportSources bad_channel;
  bad_channel.roughness = {.path = "r.png", .channel = 4};
  CHECK(ImportOrmTexture(prefab, 0, bad_channel, {}).error() ==
        cook::CookError::kInvalidArgument);
  CHECK(prefab.textures.size() == 2);
}

TEST_CASE("UsageOfSlot picks colour, linear and normal cooking") {
  CHECK(UsageOfSlot(prefab::TextureSlot::kBaseColor) ==
        cook::TextureUsage::kColor);
  CHECK(UsageOfSlot(prefab::TextureSlot::kEmissive) ==
        cook::TextureUsage::kColor);
  CHECK(UsageOfSlot(prefab::TextureSlot::kOrm) == cook::TextureUsage::kLinear);
  CHECK(UsageOfSlot(prefab::TextureSlot::kNormal) ==
        cook::TextureUsage::kNormal);
}

TEST_CASE("Summarize counts and sizes the prefab") {
  const Prefab prefab = testing::MakeSamplePrefab();
  const PrefabSummary summary = Summarize(prefab);
  CHECK(summary.node_count == 2);
  CHECK(summary.mesh_count == 1);
  CHECK(summary.submesh_count == 2);
  CHECK(summary.vertex_count == 24);
  CHECK(summary.triangle_count == 12);
  CHECK(summary.vertex_bytes == 24 * sizeof(asset::Vertex));
  CHECK(summary.index_bytes == 36 * sizeof(asset::Index));
  CHECK(summary.material_count == 2);
  CHECK(summary.texture_count == 2);
  CHECK(summary.texture_bytes == TextureByteSize(prefab.textures[0]) +
                                     TextureByteSize(prefab.textures[1]));
  CHECK(summary.collision_bytes == 77);
  CHECK(summary.storage_bytes >= summary.vertex_bytes + summary.index_bytes);
  CHECK(std::string(FormatLabel(prefab::TextureFormat::kBc5Unorm)) == "BC5");
}

TEST_CASE("UpdatePrefabBounds encloses the meshes at the rest pose") {
  Prefab prefab = testing::MakeSamplePrefab();
  prefab.hierarchy.nodes[1].rest = {.translation = {10.0f, 0.0f, 0.0f}};
  UpdatePrefabBounds(prefab);
  CHECK(prefab.bounds.min.x == doctest::Approx(9.5f));
  CHECK(prefab.bounds.max.x == doctest::Approx(10.5f));
  CHECK(prefab.bounds.min.y == doctest::Approx(-0.5f));
  prefab.meshes.clear();
  UpdatePrefabBounds(prefab);
  CHECK(prefab.bounds.max == glm::vec3(0.0f));
}

// Child lists of fake prefab files for FindChildCycle.
using FakeFiles = std::map<std::string, std::vector<std::string>>;

std::optional<std::vector<std::string>> ReadFake(const std::string& path,
                                                 void* user) {
  const FakeFiles& files = *static_cast<const FakeFiles*>(user);
  const auto found = files.find(path);
  if (found == files.end()) {
    return std::nullopt;
  }
  return found->second;
}

TEST_CASE("FindChildCycle reports cycles through nested children") {
  FakeFiles files{{"p/a.pbo", {}},
                  {"p/b.pbo", {"p/c.pbo"}},
                  {"p/c.pbo", {"p/a.pbo"}},
                  {"p/d.pbo", {"p/e.pbo"}},
                  {"p/e.pbo", {"p/d.pbo"}}};
  CHECK_FALSE(FindChildCycle("p/x.pbo", "p/b.pbo", ReadFake, &files));
  CHECK_FALSE(FindChildCycle("p/a.pbo", "p/missing.pbo", ReadFake, &files));

  const std::optional<std::string> cycle =
      FindChildCycle("p/a.pbo", "p/b.pbo", ReadFake, &files);
  REQUIRE(cycle);
  CHECK(*cycle == "cycle: a.pbo -> b.pbo -> c.pbo -> a.pbo");

  // Adding itself; separators and "." do not hide it.
  CHECK(FindChildCycle("p/a.pbo", "p/./a.pbo", ReadFake, &files));
  // A cycle among the children that does not involve the owner.
  const std::optional<std::string> existing =
      FindChildCycle("p/a.pbo", "p/d.pbo", ReadFake, &files);
  REQUIRE(existing);
  CHECK(existing->starts_with("existing cycle"));
  // Too deep.
  FakeFiles chain{{"p/1.pbo", {"p/2.pbo"}}, {"p/2.pbo", {"p/3.pbo"}}};
  CHECK(FindChildCycle("p/0.pbo", "p/1.pbo", ReadFake, &chain, 2));
}

TEST_CASE("ReimportPrefab keeps the user's edits by name") {
  Prefab edited = ThreeNodePrefab();
  edited.materials[0].roughness = 0.9f;
  edited.materials[0].uv_sets = {1, 1, 1, 1};
  edited.clips[0].loop = false;
  edited.clips[0].events.push_back({.name = "late", .time = 0.45f});
  edited.sockets.push_back({.name = "on_stock",
                            .node = 2,
                            .local = {.translation = {0.0f, 0.0f, 1.0f}}});
  edited.physics.kind = prefab::ColliderKind::kBox;
  edited.physics.dynamic = false;
  const glm::mat4 stock_socket_world =
      NodeWorldMatrix(edited, 2) * animation::ToMatrix(edited.sockets[1].local);

  // The fresh cook has the nodes in another order, no "stock", a shorter
  // clip and its own (untouched) materials and textures.
  Prefab fresh = testing::MakeSamplePrefab();
  fresh.hierarchy.nodes.insert(fresh.hierarchy.nodes.begin(),
                               {.name = "scale_root", .parent = -1});
  fresh.hierarchy.nodes[1].parent = 0;
  fresh.hierarchy.nodes[2].parent = 1;
  fresh.meshes[0].node = 2;
  fresh.clips[0].channels[0].node = 2;
  fresh.clips[0].duration = 0.4f;
  fresh.clips[0].channels[0].times = {0.0f, 0.4f};
  fresh.sockets.clear();
  fresh.lights.clear();
  fresh.children.clear();
  fresh.physics = {};
  fresh.materials[0].textures = {1, -1, -1, -1};

  const Prefab merged = ReimportPrefab(edited, fresh);
  CHECK(merged.materials[0].roughness == doctest::Approx(0.9f));
  CHECK(merged.materials[0].uv_sets[0] == 1);
  CHECK(merged.materials[0].textures[0] == 1);  // from the cook
  CHECK_FALSE(merged.clips[0].loop);
  REQUIRE(merged.clips[0].events.size() == 3);
  CHECK(merged.clips[0].events.back().time == doctest::Approx(0.4f));
  REQUIRE(merged.sockets.size() == 2);
  CHECK(merged.sockets[0].node == 2);   // "slide" found by name
  CHECK(merged.sockets[1].node == -1);  // "stock" is gone: root, same place
  CHECK(NearlyEqual(animation::ToMatrix(merged.sockets[1].local),
                    stock_socket_world));
  CHECK(merged.lights.size() == 1);
  CHECK(merged.lights[0].node == 1);  // "root"
  CHECK(merged.children.size() == 1);
  CHECK(merged.physics.kind == prefab::ColliderKind::kBox);
  CHECK(merged.physics.cooked_shape.empty());
  CHECK_FALSE(prefab::ValidatePrefab(merged));
}

TEST_CASE("CheckBeforeSave adds editor checks to ValidatePrefab") {
  Prefab prefab = testing::MakeSamplePrefab();
  CHECK_FALSE(CheckBeforeSave(prefab));
  prefab.sockets.push_back(prefab.sockets[0]);
  CHECK(CheckBeforeSave(prefab));
  prefab.sockets.pop_back();

  prefab.physics.mass = 0.0f;
  CHECK(CheckBeforeSave(prefab));
  prefab.physics.mass = 1.0f;
  prefab.physics.cooked_shape = {};
  CHECK(CheckBeforeSave(prefab));
  prefab.physics.kind = prefab::ColliderKind::kSphere;
  CHECK_FALSE(CheckBeforeSave(prefab));

  prefab.clips.push_back(prefab.clips[0]);
  CHECK(CheckBeforeSave(prefab));
}

TEST_CASE("AddSocket and AddPointLight make unique names") {
  Prefab prefab = testing::MakeSamplePrefab();
  CHECK(AddSocket(prefab, 1, "muzzle") == 1);
  CHECK(prefab.sockets[1].name == "muzzle 2");
  CHECK(prefab.sockets[1].node == 1);
  CHECK(AddPointLight(prefab, -1) == 1);
  CHECK(prefab.lights[1].name == "light");
  CHECK_FALSE(prefab::ValidatePrefab(prefab));
}

TEST_CASE("AddChildPrefab rejects bad nodes and adding itself") {
  Prefab prefab = testing::MakeSamplePrefab();
  CHECK(AddChildPrefab(prefab, "", "x.pbo", 5));
  CHECK(AddChildPrefab(prefab, "dir/self.pbo", "dir/self.pbo", -1));
  CHECK(prefab.children.size() == 1);
  CHECK_FALSE(AddChildPrefab(prefab, "dir/self.pbo", "dir/missing.pbo", 0));
  REQUIRE(prefab.children.size() == 2);
  CHECK(prefab.children[1].node == 0);
}

TEST_CASE("MoveToNode keeps the model-space placement") {
  Prefab prefab = ThreeNodePrefab();
  const glm::mat4 socket_world =
      NodeWorldMatrix(prefab, 1) * animation::ToMatrix(prefab.sockets[0].local);
  REQUIRE(MoveToNode(prefab, AttachmentKind::kSocket, 0, 2));
  CHECK(prefab.sockets[0].node == 2);
  CHECK(NearlyEqual(
      NodeWorldMatrix(prefab, 2) * animation::ToMatrix(prefab.sockets[0].local),
      socket_world));

  const glm::vec3 light_world = glm::vec3(
      NodeWorldMatrix(prefab, 0) * glm::vec4(prefab.lights[0].position, 1.0f));
  REQUIRE(MoveToNode(prefab, AttachmentKind::kLight, 0, 1));
  const glm::vec3 moved = glm::vec3(NodeWorldMatrix(prefab, 1) *
                                    glm::vec4(prefab.lights[0].position, 1.0f));
  CHECK(moved.x == doctest::Approx(light_world.x));
  CHECK(moved.y == doctest::Approx(light_world.y));
  CHECK(moved.z == doctest::Approx(light_world.z));

  REQUIRE(MoveToNode(prefab, AttachmentKind::kChild, 0, -1));
  CHECK(prefab.children[0].node == -1);
  CHECK_FALSE(MoveToNode(prefab, AttachmentKind::kSocket, 3, 0));
  CHECK_FALSE(MoveToNode(prefab, AttachmentKind::kNode, 0, 1));
  CHECK_FALSE(prefab::ValidatePrefab(prefab));
}

TEST_CASE("StoredImportOptions falls back to defaults") {
  Prefab prefab;
  CHECK(StoredImportOptions(prefab).scale == doctest::Approx(1.0f));
  cook::ModelCookOptions options;
  options.scale = 0.25f;
  options.merge_static_nodes = true;
  prefab.edit.import_settings = cook::FormatModelCookOptions(options);
  const cook::ModelCookOptions stored = StoredImportOptions(prefab);
  CHECK(stored.scale == doctest::Approx(0.25f));
  CHECK(stored.merge_static_nodes);
}

}  // namespace
}  // namespace blue::editor
