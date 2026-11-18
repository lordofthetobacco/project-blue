#include "world/world.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "render/scene_view.h"
#include "scene/scene_data.h"
#include "tests/prefab_samples.h"
#include "tests/scratch_directory.h"
#include "tests/world_fixtures.h"
#include "world/gpu_backend.h"
#include "world/prefab_cache.h"
#include "world/prefab_pose.h"

namespace blue::world {
namespace {

using testing::FakeGpu;
using testing::MakeCubePrefab;

constexpr float kEpsilon = 1.0e-4f;

bool Near(const glm::vec3& a, const glm::vec3& b) {
  return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

glm::vec3 Origin(const glm::mat4& m) { return glm::vec3(m[3]); }

// Saves `prefab` as `name` in `dir` and returns the path.
std::string SavePbo(const testing::ScratchDirectory& dir, std::string_view name,
                    const prefab::Prefab& prefab) {
  const std::string path = dir.Child(name);
  REQUIRE(prefab::SavePrefab(prefab, path).ok());
  return path;
}

scene::InstanceId Place(scene::SceneData& scene, std::string_view path,
                        glm::vec3 position, std::string name = "item",
                        scene::InstanceId parent = scene::kNoInstance) {
  scene::Instance instance;
  instance.name = std::move(name);
  instance.prefab = scene::AddPrefab(scene, path);
  instance.parent = parent;
  instance.transform.position = position;
  const scene::InstanceId id = scene::AddInstance(scene, std::move(instance));
  REQUIRE(id != scene::kNoInstance);
  return id;
}

bool HasIssue(const std::vector<WorldIssue>& issues, std::string_view text) {
  for (const WorldIssue& issue : issues) {
    if (issue.message.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST_CASE("ToMaterialDesc maps cooked material features") {
  prefab::Prefab prefab = testing::MakeSamplePrefab();
  prefab::Material& material = prefab.materials[0];
  material.textures = {0, 0, 1, -1};  // base, ORM, BC5 normal
  material.uv_sets = {0, 1, 1, 0};
  const material::MaterialDesc desc = ToMaterialDesc(material, prefab.textures);
  CHECK(desc.occlusion_in_metallic_roughness);
  CHECK(desc.normal_encoding == material::NormalEncoding::kRg);
  CHECK(desc.uv_set[static_cast<size_t>(
            material::TextureSlot::kMetallicRoughness)] == 1);
  CHECK(desc.uv_set[static_cast<size_t>(material::TextureSlot::kOcclusion)] ==
        1);
  CHECK(desc.uv_set[static_cast<size_t>(material::TextureSlot::kNormal)] == 1);
  CHECK(desc.roughness == material.roughness);
  CHECK(desc.alpha_mode == material::AlphaMode::kMask);

  const std::vector<render::TextureId> uploaded = {render::TextureId{7},
                                                   render::TextureId{8}};
  const render::MaterialTextures textures =
      ToMaterialTextures(material, uploaded);
  CHECK(textures.base_color == render::TextureId{7});
  CHECK(textures.metallic_roughness == render::TextureId{7});
  CHECK(textures.occlusion == render::TextureId{7});
  CHECK(textures.normal == render::TextureId{8});
  CHECK_FALSE(textures.emissive.has_value());

  prefab.materials[1].textures = {-1, -1, -1, -1};
  CHECK_FALSE(ToMaterialDesc(prefab.materials[1], prefab.textures)
                  .occlusion_in_metallic_roughness);
}

TEST_CASE("PrefabCache loads each file once and uploads it") {
  const testing::ScratchDirectory dir("world_cache");
  const std::string path = SavePbo(dir, "cube.pbo", MakeCubePrefab({"a", "b"}));
  FakeGpu gpu;
  PrefabCache cache(gpu);

  auto first = cache.Get(path);
  REQUIRE(first.ok());
  auto again = cache.Get(dir.Child("sub/../cube.pbo"));
  REQUIRE(again.ok());
  CHECK(first.value() == again.value());
  CHECK(cache.size() == 1);
  CHECK(gpu.mesh_uploads == 1);
  CHECK(gpu.descs.size() == 2);
  const PrefabGpu& uploaded = cache.gpu(first.value());
  REQUIRE(uploaded.submeshes.size() == 1);
  REQUIRE(uploaded.submeshes[0].size() == 2);
  CHECK(uploaded.submeshes[0][1].material == uploaded.materials[1]);
  CHECK(cache.Find(path) == first.value());

  auto missing = cache.Get(dir.Child("missing.pbo"));
  REQUIRE_FALSE(missing.ok());
  CHECK(missing.error() == prefab::PrefabError::kCannotOpen);

  // A prefab without meshes (a material library) is fine.
  prefab::Prefab library;
  library.materials.push_back({.name = "gold"});
  auto library_id = cache.Get(SavePbo(dir, "library.pbo", library));
  REQUIRE(library_id.ok());
  CHECK(cache.gpu(library_id.value()).meshes.empty());
}

TEST_CASE("PrefabCache replaces, updates and releases GPU resources") {
  FakeGpu gpu;
  PrefabCache cache(gpu);
  const PrefabId id = cache.Upload(testing::MakeSamplePrefab(), "doc.pbo");
  const PrefabGpu before = cache.gpu(id);
  const size_t bytes = cache.gpu_bytes();
  CHECK(bytes > 0);
  CHECK(before.textures.size() == 2);

  // Parameter edit: no upload, one in-place update.
  prefab::Material edited = cache.prefab(id).materials[0];
  edited.roughness = 0.9f;
  cache.UpdateMaterial(id, 0, edited);
  REQUIRE(gpu.updates.size() == 1);
  CHECK(gpu.updates[0].first == before.materials[0]);
  CHECK(gpu.updates[0].second.roughness == 0.9f);
  CHECK(gpu.mesh_uploads == 1);
  CHECK(cache.prefab(id).materials[0].roughness == 0.9f);

  // Same key: replaced in place, old resources released, bytes balanced.
  CHECK(cache.Upload(testing::MakeSamplePrefab(), "doc.pbo") == id);
  CHECK(gpu.released_meshes == before.meshes);
  CHECK(gpu.released_textures == before.textures);
  CHECK(gpu.released_materials == before.materials);
  CHECK(cache.gpu_bytes() == bytes);
  CHECK(cache.gpu(id).meshes != before.meshes);

  cache.Release(id);
  CHECK_FALSE(cache.alive(id));
  CHECK(cache.gpu_bytes() == 0);
  CHECK(gpu.released_meshes.size() == 2);
  cache.Release(id);  // no-op
  CHECK(gpu.released_meshes.size() == 2);
  CHECK_FALSE(cache.Find("doc.pbo").has_value());
}

TEST_CASE("PrefabPose plays clips, fires events and places sockets") {
  FakeGpu gpu;
  PrefabCache cache(gpu);
  const PrefabId id = cache.Upload(testing::MakeSamplePrefab(), "gun.pbo");
  PrefabPose pose(cache, id);
  const glm::vec3 rest = Origin(pose.NodeMatrix(1));

  std::optional<glm::mat4> muzzle = pose.SocketMatrix("muzzle");
  REQUIRE(muzzle.has_value());
  CHECK_FALSE(pose.SocketMatrix("missing").has_value());

  REQUIRE(pose.Play("fire", 0.0f, 1.0f, /*loop=*/false));
  CHECK_FALSE(pose.Play("missing"));
  // First update fires the event at time 0.
  CHECK(pose.Update(0.1f));
  REQUIRE(pose.fired_events().size() == 1);
  CHECK(pose.fired_events()[0]->name == "shot");
  CHECK(pose.Update(0.15f));  // 0.1 -> 0.25 crosses slide_lock at 0.2
  REQUIRE(pose.fired_events().size() == 1);
  CHECK(pose.fired_events()[0]->name == "slide_lock");
  CHECK(pose.Update(0.1f));
  CHECK(pose.fired_events().empty());
  // The slide moved back along -z of its parent space.
  CHECK_FALSE(Near(Origin(pose.NodeMatrix(1)), rest));
  CHECK(Near(
      Origin(*pose.SocketMatrix("muzzle")),
      Origin(pose.NodeMatrix(1) * glm::mat4(1.0f)) +
          glm::vec3(pose.NodeMatrix(1) * glm::vec4(0.0f, 0.0f, -0.3f, 0.0f))));

  // Looping across the end fires the start event again.
  REQUIRE(pose.Play("fire", 0.0f, 1.0f, /*loop=*/true));
  pose.Update(0.45f);
  pose.Update(0.1f);  // 0.45 -> 0.05 wraps over 0
  REQUIRE(pose.fired_events().size() == 1);
  CHECK(pose.fired_events()[0]->name == "shot");

  // The clip drives the slide from the origin of its parent at t = 0.
  pose.SetTime(0.0f);
  CHECK(Near(Origin(pose.NodeMatrix(1)), {0, 0, 0}));
  pose.ResetToRest();
  CHECK(Near(Origin(pose.NodeMatrix(1)), rest));
  CHECK(pose.clip() == -1);
  CHECK_FALSE(pose.Update(0.1f));

  std::vector<render::DrawItem> draws;
  pose.AppendDraws(glm::mat4(1.0f), draws);
  REQUIRE(draws.size() == 1);
  CHECK(draws[0].mesh == cache.gpu(id).meshes[0]);
  CHECK(draws[0].submeshes.size() == 2);
}

TEST_CASE("World resolves instances, groups, visibility and lights") {
  const testing::ScratchDirectory dir("world_build");
  prefab::Prefab lamp = MakeCubePrefab({"metal"});
  lamp.lights.push_back(
      {.name = "bulb", .node = 0, .position = {0, 1, 0}, .range = 5.0f});
  const std::string lamp_path = SavePbo(dir, "lamp.pbo", lamp);
  const std::string cube_path = SavePbo(dir, "cube.pbo", MakeCubePrefab({"a"}));

  scene::SceneData scene;
  scene::Instance group{.name = "group"};
  group.transform.position = {10, 0, 0};
  const scene::InstanceId group_id = scene::AddInstance(scene, group);
  const scene::InstanceId lamp_id =
      Place(scene, lamp_path, {1, 0, 0}, "lamp", group_id);
  Place(scene, cube_path, {0, 0, 5}, "cube");
  scene::Instance hidden_group{.name = "hidden", .visible = false};
  const scene::InstanceId hidden_id = scene::AddInstance(scene, hidden_group);
  Place(scene, cube_path, {0, 0, 0}, "under hidden", hidden_id);
  scene.point_lights.push_back({.name = "sky", .position = {0, 9, 0}});

  FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  CHECK(world.Build(scene).empty());

  CHECK(world.placements().size() == 2);
  std::vector<render::DrawItem> draws;
  world.AppendDraws(draws);
  CHECK(draws.size() == 2);
  CHECK(Near(Origin(draws[0].transform), {11, 0, 0}));
  CHECK(Near(Origin(draws[1].transform), {0, 0, 5}));

  REQUIRE(world.point_lights().size() == 2);
  CHECK(Near(world.point_lights()[0].position, {0, 9, 0}));
  CHECK(Near(world.point_lights()[1].position, {11, 1, 0}));
  CHECK(world.point_lights()[1].range == 5.0f);

  // Moving the group moves the lamp, its light and nothing else.
  scene::FindInstance(scene, group_id)->transform.position = {20, 0, 0};
  CHECK(world.SetInstanceTransform(scene, group_id));
  draws.clear();
  world.AppendDraws(draws);
  CHECK(Near(Origin(draws[0].transform), {21, 0, 0}));
  CHECK(Near(Origin(draws[1].transform), {0, 0, 5}));
  CHECK(Near(world.point_lights()[1].position, {21, 1, 0}));
  CHECK_FALSE(world.SetInstanceTransform(scene, hidden_id));

  // Selecting the group selects the lamp's draw.
  std::vector<uint32_t> selected;
  world.AppendSelectedDraws(std::vector<scene::InstanceId>{group_id}, 3,
                            selected);
  CHECK(selected == std::vector<uint32_t>{3});

  const std::optional<asset::Bounds> bounds = world.InstanceBounds(lamp_id);
  REQUIRE(bounds.has_value());
  CHECK(Near(bounds->min, {20.5f, -0.5f, -0.5f}));
  CHECK(Near(bounds->max, {21.5f, 0.5f, 0.5f}));
  CHECK_FALSE(world.InstanceBounds(group_id).has_value());
}

TEST_CASE(
    "World nests child prefabs and reports cycles, depth and "
    "missing files") {
  const testing::ScratchDirectory dir("world_nested");
  prefab::Prefab child = MakeCubePrefab({"child"});
  child.sockets.push_back(
      {.name = "tip", .node = 0, .local = {.translation = {0, 2, 0}}});
  const std::string child_path = SavePbo(dir, "child.pbo", child);
  prefab::Prefab parent = MakeCubePrefab({"parent"});
  parent.children.push_back(
      {.path = child_path, .node = 0, .local = {.translation = {0, 0, 3}}});
  const std::string parent_path = SavePbo(dir, "parent.pbo", parent);

  scene::SceneData scene;
  const scene::InstanceId id = Place(scene, parent_path, {1, 0, 0});
  FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  CHECK(world.Build(scene).empty());
  REQUIRE(world.placements().size() == 2);
  CHECK(world.placements()[1].parent == 0);
  CHECK(world.placements()[1].depth == 1);
  CHECK(Near(Origin(world.placements()[1].world), {1, 0, 3}));
  // Sockets of nested prefabs are found too.
  const std::optional<glm::mat4> tip = world.SocketMatrix(id, "tip");
  REQUIRE(tip.has_value());
  CHECK(Near(Origin(*tip), {1, 2, 3}));
  const auto names = world.MaterialNames(scene);
  REQUIRE(names.size() == 1);
  CHECK(names[0] == std::vector<std::string>{"parent", "child"});

  // a.pbo -> b.pbo -> a.pbo
  const std::string a_path = dir.Child("a.pbo");
  const std::string b_path = dir.Child("b.pbo");
  prefab::Prefab a = MakeCubePrefab({"a"});
  a.children.push_back({.path = b_path});
  prefab::Prefab b = MakeCubePrefab({"b"});
  b.children.push_back({.path = a_path});
  SavePbo(dir, "a.pbo", a);
  SavePbo(dir, "b.pbo", b);
  scene::SceneData cyclic;
  Place(cyclic, a_path, {0, 0, 0});
  Place(cyclic, dir.Child("nowhere.pbo"), {0, 0, 0});
  const std::vector<WorldIssue> issues = world.Build(cyclic);
  CHECK(HasIssue(issues, "nested prefab cycle"));
  CHECK(HasIssue(issues, "a.pbo -> "));
  CHECK(HasIssue(issues, "cannot load prefab"));
  CHECK(world.placements().size() == 2);  // a and b once each

  // A chain deeper than the limit stops at it.
  std::string previous;
  for (int level = kMaxPrefabDepth + 2; level >= 0; --level) {
    prefab::Prefab link = MakeCubePrefab({"x"});
    if (!previous.empty()) {
      link.children.push_back({.path = previous});
    }
    previous = SavePbo(dir, std::format("deep{}.pbo", level), link);
  }
  scene::SceneData deep;
  Place(deep, previous, {0, 0, 0});
  CHECK(HasIssue(world.Build(deep), "depth limit"));
  CHECK(world.placements().size() == static_cast<size_t>(kMaxPrefabDepth + 1));
}

TEST_CASE("World applies material overrides once per distinct override") {
  const testing::ScratchDirectory dir("world_override");
  const std::string sphere_path =
      SavePbo(dir, "sphere.pbo", MakeCubePrefab({"sphere", "trim"}));
  prefab::Prefab library;
  library.materials.push_back({.name = "gold", .metallic = 1.0f});
  const std::string library_path = SavePbo(dir, "materials.pbo", library);

  scene::SceneData scene;
  const int library_index = scene::AddPrefab(scene, library_path);
  std::vector<scene::InstanceId> ids;
  for (int i = 0; i < 3; ++i) {
    ids.push_back(Place(scene, sphere_path, {static_cast<float>(i), 0, 0}));
  }
  // Two instances share a by-reference override, one tweaks roughness.
  for (int i = 0; i < 2; ++i) {
    scene::FindInstance(scene, ids[static_cast<size_t>(i)])
        ->materials.push_back({.material_name = "sphere",
                               .source_prefab = library_index,
                               .source_material = "gold"});
  }
  scene::FindInstance(scene, ids[2])
      ->materials.push_back({.material_name = "sphere", .roughness = 0.1f});

  FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  CHECK(world.Build(scene).empty());
  std::vector<render::DrawItem> draws;
  world.AppendDraws(draws);
  REQUIRE(draws.size() == 3);
  const PrefabId sphere = *cache.Find(sphere_path);
  const render::MaterialId own = cache.gpu(sphere).materials[0];
  const render::MaterialId trim = cache.gpu(sphere).materials[1];
  CHECK(draws[0].submeshes[0].material != own);
  CHECK(draws[0].submeshes[0].material == draws[1].submeshes[0].material);
  CHECK(draws[0].submeshes[1].material == trim);  // not overridden
  CHECK(draws[2].submeshes[0].material != own);
  CHECK(draws[2].submeshes[0].material != draws[0].submeshes[0].material);

  // The derived materials: gold's parameters, and sphere with roughness.
  const auto desc_of = [&gpu](render::MaterialId id) {
    return gpu.descs[static_cast<size_t>(id) - 1];
  };
  CHECK(desc_of(draws[0].submeshes[0].material).metallic == 1.0f);
  CHECK(desc_of(draws[2].submeshes[0].material).roughness == 0.1f);

  // Rebuilding reuses the derived materials.
  const size_t created = gpu.descs.size();
  CHECK(world.Build(scene).empty());
  CHECK(gpu.descs.size() == created);

  // Unknown names are reported, not fatal.
  scene::FindInstance(scene, ids[2])
      ->materials.push_back({.material_name = "nope", .metallic = 0.5f});
  scene::FindInstance(scene, ids[1])->materials[0].source_material = "silver";
  const std::vector<WorldIssue> issues = world.Build(scene);
  CHECK(HasIssue(issues, "'nope': no such material"));
  CHECK(HasIssue(issues, "'silver' not found"));
}

TEST_CASE("World picks instances by bounds and by triangles") {
  const testing::ScratchDirectory dir("world_pick");
  const std::string cube_path = SavePbo(dir, "cube.pbo", MakeCubePrefab({"a"}));
  scene::SceneData scene;
  const scene::InstanceId near_id = Place(scene, cube_path, {0, 0, -5});
  const scene::InstanceId far_id = Place(scene, cube_path, {0, 0, -10});
  scene::Instance& rotated = *scene::FindInstance(scene, far_id);
  rotated.transform.rotation =
      glm::angleAxis(0.7853982f, glm::vec3(0.0f, 1.0f, 0.0f));
  FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  REQUIRE(world.Build(scene).empty());

  for (const PickMode mode : {PickMode::kBounds, PickMode::kTriangles}) {
    std::optional<PickHit> hit =
        world.Pick({0, 0, 0}, {0, 0, -2}, 100.0f, mode);
    REQUIRE(hit.has_value());
    CHECK(hit->instance == near_id);
    CHECK(hit->distance == doctest::Approx(4.5f));
    CHECK(Near(hit->point, {0, 0, -4.5f}));

    // Past the near cube, along x = 0.65: misses the near cube, hits the
    // rotated far one (its corner reaches x = 0.707).
    hit = world.Pick({0.65f, 0, 0}, {0, 0, -1}, 100.0f, mode);
    REQUIRE(hit.has_value());
    CHECK(hit->instance == far_id);
    CHECK_FALSE(world.Pick({0, 5, 0}, {0, 0, -1}, 100.0f, mode).has_value());
    CHECK_FALSE(world.Pick({0, 0, 0}, {0, 0, -1}, 3.0f, mode).has_value());
  }
  // kBounds tests the oriented mesh box: for a cube it matches the
  // triangles.
  const std::optional<PickHit> by_bounds =
      world.Pick({0.69f, 0, 0}, {0, 0, -1}, 100.0f, PickMode::kBounds);
  const std::optional<PickHit> by_triangles =
      world.Pick({0.69f, 0, 0}, {0, 0, -1}, 100.0f, PickMode::kTriangles);
  REQUIRE(by_bounds.has_value());
  REQUIRE(by_triangles.has_value());
  CHECK(by_bounds->distance == doctest::Approx(by_triangles->distance));
}

TEST_CASE("World animates instances and reports broken parents") {
  const testing::ScratchDirectory dir("world_anim");
  const std::string gun_path =
      SavePbo(dir, "gun.pbo", testing::MakeSamplePrefab());
  scene::SceneData scene;
  const scene::InstanceId gun = Place(scene, gun_path, {0, 0, 0});
  scene::Instance orphan{.name = "orphan", .parent = 999};
  scene.instances.push_back(orphan);
  scene.instances.back().id = scene.next_instance_id++;

  FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  CHECK(HasIssue(world.Build(scene), "broken parent chain"));
  std::vector<render::DrawItem> rest;
  world.AppendDraws(rest);

  CHECK(world.PlayClip(gun, "fire"));
  CHECK_FALSE(world.PlayClip(gun, "missing"));
  world.Update(0.25f);
  std::vector<render::DrawItem> moved;
  world.AppendDraws(moved);
  REQUIRE(moved.size() == rest.size());
  CHECK_FALSE(Near(Origin(moved[0].transform), Origin(rest[0].transform)));
  PrefabPose* pose = world.pose(gun);
  REQUIRE(pose != nullptr);
  pose->SetTime(0.0f);  // scrubbed: the slide at its parent's origin
  world.Update(0.0f);
  moved.clear();
  world.AppendDraws(moved);
  CHECK(Near(Origin(moved[0].transform), {0, 0, 0}));
  pose->ResetToRest();
  world.Update(0.0f);
  moved.clear();
  world.AppendDraws(moved);
  CHECK(Near(Origin(moved[0].transform), Origin(rest[0].transform)));
}

TEST_CASE("World build time for a few hundred instances") {
  const testing::ScratchDirectory dir("world_timing");
  const std::string cube_path =
      SavePbo(dir, "cube.pbo", MakeCubePrefab({"a", "b"}));
  scene::SceneData scene;
  for (int i = 0; i < 300; ++i) {
    const scene::InstanceId id =
        Place(scene, cube_path, {static_cast<float>(i), 0, 0});
    if (i % 3 == 0) {
      scene::FindInstance(scene, id)->materials.push_back(
          {.material_name = "a", .roughness = 0.25f});
    }
  }
  FakeGpu gpu;
  PrefabCache cache(gpu);
  World world(cache);
  REQUIRE(world.Build(scene).empty());  // loads the prefab once
  const auto start = std::chrono::steady_clock::now();
  REQUIRE(world.Build(scene).empty());
  const double build_ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start)
                              .count();
  std::vector<render::DrawItem> draws;
  draws.reserve(300);
  world.AppendDraws(draws);
  CHECK(draws.size() == 300);
  MESSAGE("World::Build of 300 instances (cached prefab): ", build_ms, " ms");
}

}  // namespace blue::world
