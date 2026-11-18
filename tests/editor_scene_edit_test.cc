#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "editor/scene_mode/scene_edit.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "tests/world_fixtures.h"
#include "world/prefab_cache.h"
#include "world/world.h"

namespace blue::editor {
namespace {

using scene::InstanceId;
using scene::kNoInstance;

std::vector<InstanceId> RowIds(const std::vector<OutlinerRow>& rows) {
  std::vector<InstanceId> ids;
  for (const OutlinerRow& row : rows) {
    ids.push_back(row.id);
  }
  return ids;
}

scene::Instance Group(std::string name, InstanceId parent = kNoInstance) {
  scene::Instance instance;
  instance.name = std::move(name);
  instance.parent = parent;
  return instance;
}

}  // namespace

TEST_CASE("PlacementPoint lands on the ground or in front of the camera") {
  // Looking down at 45 degrees from 2 m: ground 2 m ahead.
  const glm::vec3 down = glm::normalize(glm::vec3(0.0f, -1.0f, -1.0f));
  const glm::vec3 ground = PlacementPoint(glm::vec3(0.0f, 2.0f, 0.0f), down);
  CHECK(ground.y == 0.0f);
  CHECK(ground.z == doctest::Approx(-2.0f));

  // A nearer picked surface wins.
  const glm::vec3 surface =
      PlacementPoint(glm::vec3(0.0f, 2.0f, 0.0f), down, 1.0f);
  CHECK(surface.y == doctest::Approx(2.0f - 0.7071f).epsilon(1e-3));

  // Looking up: the fallback distance along the ray.
  const glm::vec3 up =
      PlacementPoint(glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  CHECK(up.y == doctest::Approx(2.0f + kPlacementDistance));

  // Ground beyond max_distance: fallback.
  const glm::vec3 shallow =
      PlacementPoint(glm::vec3(0.0f, 1.0f, 0.0f),
                     glm::normalize(glm::vec3(0.0f, -0.001f, -1.0f)),
                     std::nullopt, 50.0f, 3.0f);
  CHECK(shallow.z == doctest::Approx(-3.0f).epsilon(1e-3));
}

TEST_CASE("PlacePrefab names, registers and positions instances") {
  scene::SceneData scene;
  const InstanceId a =
      PlacePrefab(scene, "C:/assets/crate.pbo", {1.0f, 0.0f, 2.0f});
  const InstanceId b =
      PlacePrefab(scene, "C:/assets/crate.pbo", {3.0f, 0.0f, 2.0f});
  REQUIRE(a != kNoInstance);
  REQUIRE(b != kNoInstance);
  CHECK(scene.prefabs.size() == 1);
  CHECK(scene::FindInstance(scene, a)->name == "crate");
  CHECK(scene::FindInstance(scene, b)->name == "crate 2");
  CHECK(scene::FindInstance(scene, b)->transform.position.x == 3.0f);

  // Under a moved parent the position is converted into its space.
  scene::Instance parent = Group("group");
  parent.transform.position = {10.0f, 0.0f, 0.0f};
  const InstanceId group = scene::AddInstance(scene, parent);
  const InstanceId child =
      PlacePrefab(scene, "C:/assets/rock.pbo", {12.0f, 0.0f, 0.0f}, group);
  CHECK(scene::FindInstance(scene, child)->transform.position.x ==
        doctest::Approx(2.0f));
  CHECK(PlacePrefab(scene, "", glm::vec3(0.0f)) == kNoInstance);
  CHECK(PlacePrefab(scene, "C:/x.pbo", glm::vec3(0.0f), 99) == kNoInstance);
  CHECK_FALSE(scene::ValidateSceneData(scene).has_value());
}

TEST_CASE("DeleteInstances and DuplicateInstances handle nested selections") {
  scene::SceneData scene;
  const InstanceId group = scene::AddInstance(scene, Group("group"));
  const InstanceId child =
      PlacePrefab(scene, "C:/assets/crate.pbo", glm::vec3(0.0f), group);
  const InstanceId other =
      PlacePrefab(scene, "C:/assets/rock.pbo", {5.0f, 0.0f, 0.0f});

  const std::vector<InstanceId> both = {child, group};
  const std::vector<InstanceId> copies =
      DuplicateInstances(scene, both, {0.0f, 0.0f, 1.0f});
  REQUIRE(copies.size() == 1);  // child is copied with its group
  const scene::Instance* copy = scene::FindInstance(scene, copies[0]);
  CHECK(copy->name == "group 2");
  CHECK(copy->transform.position.z == doctest::Approx(1.0f));
  CHECK(scene::ChildrenOf(scene, copies[0]).size() == 1);
  CHECK(scene.instances.size() == 5);

  const std::vector<InstanceId> doomed = {child, group, other, 42};
  CHECK(DeleteInstances(scene, doomed) == 2);
  CHECK(scene.instances.size() == 2);  // the copy and its child
  CHECK(scene.prefabs.size() == 1);    // rock compacted away
  CHECK_FALSE(scene::ValidateSceneData(scene).has_value());
}

TEST_CASE("OutlinerRows lists the forest depth first") {
  scene::SceneData scene;
  const InstanceId a = scene::AddInstance(scene, Group("a"));
  const InstanceId b = scene::AddInstance(scene, Group("b"));
  const InstanceId a1 = scene::AddInstance(scene, Group("a1", a));
  const InstanceId a1x = scene::AddInstance(scene, Group("a1x", a1));
  const InstanceId a2 = scene::AddInstance(scene, Group("a2", a));
  // A child stored before its parent still lands below it.
  scene.instances.insert(scene.instances.begin(), scene.instances.back());
  scene.instances.pop_back();

  const std::vector<OutlinerRow> rows = OutlinerRows(scene);
  // a2 is stored before a1 now, so it is listed first among a's children.
  CHECK(RowIds(rows) == std::vector<InstanceId>{a, a2, a1, a1x, b});
  CHECK(rows[0].depth == 0);
  CHECK(rows[0].has_children);
  CHECK(rows[3].depth == 2);
  CHECK_FALSE(rows[4].has_children);

  const std::vector<InstanceId> collapsed = {a1};
  CHECK(RowIds(OutlinerRows(scene, collapsed)) ==
        std::vector<InstanceId>{a, a2, a1, b});

  CHECK(StepInstance(scene, a1x, 1) == b);
  CHECK(StepInstance(scene, b, 1) == a);  // wraps
  CHECK(StepInstance(scene, a, -1) == b);
  CHECK(StepInstance(scene, 99, 1) == a);
  CHECK(StepInstance(scene::SceneData{}, a, 1) == kNoInstance);
  CHECK(FindInstanceByName(scene, "a1x") == a1x);
  CHECK(FindInstanceByName(scene, "nope") == kNoInstance);
}

TEST_CASE("Material override editing keeps the prefab table compact") {
  scene::SceneData scene;
  const InstanceId id =
      PlacePrefab(scene, "C:/assets/sphere.pbo", glm::vec3(0.0f));

  scene::MaterialOverride& tweak =
      FindOrAddMaterialOverride(*scene::FindInstance(scene, id), "paint");
  CHECK(IsEmptyOverride(tweak));
  tweak.roughness = 0.3f;
  CHECK_FALSE(IsEmptyOverride(tweak));
  CHECK(&FindOrAddMaterialOverride(*scene::FindInstance(scene, id), "paint") ==
        &scene::FindInstance(scene, id)->materials[0]);

  REQUIRE(SetMaterialReplacement(scene, id, "paint", "C:/assets/materials.pbo",
                                 "steel"));
  CHECK(scene.prefabs.size() == 2);
  const scene::MaterialOverride* replaced =
      scene::FindMaterialOverride(scene, id, "paint");
  REQUIRE(replaced != nullptr);
  CHECK(replaced->source_prefab == 1);
  CHECK(replaced->source_material == "steel");
  CHECK(replaced->roughness == 0.3f);  // tweaks kept
  CHECK_FALSE(scene::ValidateSceneData(scene).has_value());

  CHECK_FALSE(SetMaterialReplacement(scene, id, "paint",
                                     "C:/assets/materials.pbo", ""));
  REQUIRE(SetMaterialReplacement(scene, id, "paint", "", ""));
  CHECK(scene.prefabs.size() == 1);  // library no longer used
  CHECK(scene::FindMaterialOverride(scene, id, "paint")->source_prefab ==
        scene::kNoPrefab);

  CHECK(RemoveMaterialOverride(scene, id, "paint"));
  CHECK_FALSE(RemoveMaterialOverride(scene, id, "paint"));
  CHECK(scene::FindInstance(scene, id)->materials.empty());
  CHECK_FALSE(scene::ValidateSceneData(scene).has_value());
}

}  // namespace blue::editor

namespace blue::editor {

// Times what Scene mode does on the cooked sample range: a warm rebuild
// (World::Build + MaterialNames + ValidateAgainstPrefabs, after every
// committed edit), the per-frame save check of the Problems panel, and a
// triangle pick (placement and clicks).
TEST_CASE("Scene mode work on the sample range is timed") {
  auto loaded = scene::LoadMscn(BLUE_ASSETS_DIR "/range.mscn");
  REQUIRE(loaded.ok());
  const scene::SceneData& scene = loaded.value();
  testing::FakeGpu gpu;
  world::PrefabCache cache(gpu);
  world::World world(cache);
  REQUIRE(world.Build(scene).empty());  // loads and caches the prefabs

  using Clock = std::chrono::steady_clock;
  const auto ms_since = [](Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start)
        .count();
  };
  constexpr int kRuns = 20;
  auto start = Clock::now();
  for (int i = 0; i < kRuns; ++i) {
    CHECK(world.Build(scene).empty());
    CHECK(scene::ValidateAgainstPrefabs(scene, world.MaterialNames(scene))
              .empty());
  }
  const double rebuild_ms = ms_since(start) / kRuns;

  start = Clock::now();
  for (int i = 0; i < kRuns; ++i) {
    CHECK_FALSE(
        scene::CheckMscnSerializable(scene, BLUE_ASSETS_DIR "/range.mscn")
            .has_value());
  }
  const double check_ms = ms_since(start) / kRuns;

  const glm::vec3 eye = scene.player_start.position;
  start = Clock::now();
  std::optional<world::PickHit> hit;
  for (int i = 0; i < kRuns; ++i) {
    hit = world.Pick(eye, glm::vec3(0.0f, -0.3f, -1.0f), 10000.0f,
                     world::PickMode::kTriangles);
  }
  const double pick_ms = ms_since(start) / kRuns;
  CHECK(hit.has_value());

  MESSAGE("range.mscn in Scene mode: rebuild ", rebuild_ms, " ms, save check ",
          check_ms, " ms, triangle pick ", pick_ms, " ms");
  // Generous bounds for Debug builds; a regression to per-frame seconds
  // would fail.
  CHECK(rebuild_ms < 50.0);
  CHECK(check_ms < 5.0);
}

}  // namespace blue::editor
