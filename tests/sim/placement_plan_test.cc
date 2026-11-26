#include "world/core/placement_plan.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "animation/transform.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/scene_data.h"
#include "tests/scratch_directory.h"
#include "world/core/placement.h"
#include "world/core/prefab_id.h"
#include "world/core/prefab_store.h"

namespace blue::world {
namespace {

// A prefab with a root node at `rest_offset` (and a second node below it)
// nesting `children` (paths) on node 1 at `child_offset`.
prefab::Prefab MakePrefab(std::vector<std::string> children = {},
                          glm::vec3 rest_offset = glm::vec3(0.0f),
                          glm::vec3 child_offset = glm::vec3(0.0f)) {
  prefab::Prefab prefab;
  prefab.hierarchy.nodes = {
      {.name = "root", .parent = -1, .rest = {.translation = rest_offset}},
      {.name = "mount",
       .parent = 0,
       .rest = {.translation = glm::vec3(0.0f, 1.0f, 0.0f)}}};
  for (std::string& path : children) {
    prefab.children.push_back({.path = std::move(path),
                               .node = 1,
                               .local = {.translation = child_offset}});
  }
  return prefab;
}

scene::Instance MakeInstance(scene::InstanceId id, int prefab,
                             glm::vec3 position = glm::vec3(0.0f)) {
  scene::Instance instance;
  instance.id = id;
  instance.name = std::format("instance {}", id);
  instance.prefab = prefab;
  instance.transform.position = position;
  return instance;
}

bool SameMatrix(const glm::mat4& a, const glm::mat4& b) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (a[column][row] != b[column][row]) {
        return false;
      }
    }
  }
  return true;
}

TEST_CASE("PrefabStore loads each file once and keys it by absolute path") {
  testing::ScratchDirectory dir("prefab_store");
  const std::string path = dir.Child("box.pbo");
  prefab::Prefab box = MakePrefab();
  box.physics.kind = prefab::ColliderKind::kBox;
  REQUIRE(prefab::SavePrefab(box, path).ok());

  PrefabStore store;
  auto first = store.Get(path);
  REQUIRE(first.ok());
  auto again = store.Get(path);
  REQUIRE(again.ok());
  CHECK(first.value() == again.value());
  CHECK(store.size() == 1);
  CHECK(store.Find(path) == first.value());
  CHECK(store.prefab(first.value()).physics.kind == prefab::ColliderKind::kBox);
  CHECK(store.prefab(first.value()).hierarchy.nodes.size() == 2);

  CHECK_FALSE(store.Get(dir.Child("missing.pbo")).ok());
  CHECK(store.size() == 1);
  CHECK_FALSE(store.Find(dir.Child("missing.pbo")).has_value());

  // Add() under an already stored key keeps the id.
  const PrefabId replaced = store.Add(MakePrefab(), path);
  CHECK(replaced == first.value());
  CHECK(store.prefab(replaced).physics.kind == prefab::ColliderKind::kNone);
}

TEST_CASE("Placements are depth first with rest-pose world matrices") {
  testing::ScratchDirectory dir("placement_plan");
  const std::string outer = dir.Child("outer.pbo");
  const std::string inner = dir.Child("inner.pbo");
  PrefabStore store;
  (void)store.Add(MakePrefab({inner, inner}, glm::vec3(0.0f, 0.0f, 2.0f),
                             glm::vec3(0.5f, 0.0f, 0.0f)),
                  outer);
  (void)store.Add(MakePrefab(), inner);

  scene::SceneData scene;
  scene.prefabs = {outer};
  scene.instances = {MakeInstance(1, 0, glm::vec3(10.0f, 0.0f, 0.0f))};
  scene.instances[0].transform.scale = glm::vec3(2.0f);

  const PlacementPlan plan = ResolvePlacements(scene, store);
  CHECK(plan.issues.empty());
  REQUIRE(plan.instances.size() == 1);
  CHECK(plan.instances[0].first_placement == 0);
  CHECK(plan.instances[0].placement_count == 3);
  REQUIRE(plan.placements.size() == 3);
  REQUIRE(plan.attachments.size() == 3);

  const glm::mat4 instance_world = scene.instances[0].transform.Matrix();
  CHECK(SameMatrix(plan.instances[0].world, instance_world));
  CHECK(SameMatrix(plan.placements[0].world, instance_world));
  CHECK(plan.placements[0].parent == -1);
  CHECK(plan.placements[0].depth == 0);
  CHECK(plan.attachments[0].parent_node == -1);

  // Node 1 of the outer prefab at rest: root (0, 0, 2) then (0, 1, 0).
  const glm::mat4 node = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, 2)) *
                         glm::translate(glm::mat4(1.0f), glm::vec3(0, 1, 0));
  const glm::mat4 local = animation::ToMatrix({.translation = {0.5f, 0, 0}});
  for (size_t i = 1; i < 3; ++i) {
    CAPTURE(i);
    const Placement& child = plan.placements[i];
    CHECK(child.owner == 1);
    CHECK(child.parent == 0);
    CHECK(child.depth == 1);
    CHECK(plan.attachments[i].parent_node == 1);
    CHECK(SameMatrix(plan.attachments[i].local, local));
    const glm::vec3 origin = glm::vec3(child.world[3]);
    CHECK(origin.x == doctest::Approx(11.0f));
    CHECK(origin.y == doctest::Approx(2.0f));
    CHECK(origin.z == doctest::Approx(4.0f));
    CHECK(SameMatrix(child.world, instance_world * node * local));
  }
}

TEST_CASE("Nested prefab cycles are cut and named") {
  testing::ScratchDirectory dir("placement_cycle");
  const std::string a = dir.Child("a.pbo");
  const std::string b = dir.Child("b.pbo");
  PrefabStore store;
  const PrefabId a_id = store.Add(MakePrefab({b}), a);
  const PrefabId b_id = store.Add(MakePrefab({a}), b);

  scene::SceneData scene;
  scene.prefabs = {a};
  scene.instances = {MakeInstance(7, 0)};
  const PlacementPlan plan = ResolvePlacements(scene, store);
  REQUIRE(plan.placements.size() == 2);
  CHECK(plan.placements[0].prefab == a_id);
  CHECK(plan.placements[1].prefab == b_id);
  REQUIRE(plan.issues.size() == 1);
  CHECK(plan.issues[0].instance == 7);
  CHECK(plan.issues[0].message == "nested prefab cycle: " + store.key(a_id) +
                                      " -> " + store.key(b_id) + " -> " + a);
}

TEST_CASE("Nested prefabs deeper than the limit are dropped") {
  testing::ScratchDirectory dir("placement_depth");
  PrefabStore store;
  constexpr int kChain = kMaxPrefabDepth + 3;
  for (int i = 0; i < kChain; ++i) {
    std::vector<std::string> children;
    if (i + 1 < kChain) {
      children.push_back(dir.Child(std::format("p{}.pbo", i + 1)));
    }
    (void)store.Add(MakePrefab(std::move(children)),
                    dir.Child(std::format("p{}.pbo", i)));
  }
  scene::SceneData scene;
  scene.prefabs = {dir.Child("p0.pbo")};
  scene.instances = {MakeInstance(1, 0)};

  const PlacementPlan plan = ResolvePlacements(scene, store);
  REQUIRE(plan.placements.size() == kMaxPrefabDepth + 1);
  CHECK(plan.placements.back().depth == kMaxPrefabDepth);
  REQUIRE(plan.issues.size() == 1);
  CHECK(plan.issues[0].message ==
        std::format("nested prefab {} exceeds the depth limit of {}",
                    dir.Child(std::format("p{}.pbo", kMaxPrefabDepth + 1)),
                    kMaxPrefabDepth));
}

TEST_CASE("Hidden instances, broken parents and missing prefabs are skipped") {
  testing::ScratchDirectory dir("placement_skip");
  const std::string box = dir.Child("box.pbo");
  PrefabStore store;
  (void)store.Add(MakePrefab(), box);

  scene::SceneData scene;
  scene.prefabs = {box, dir.Child("missing.pbo")};
  // Listed child first: the plan still places parents first.
  scene::Instance child = MakeInstance(2, 0, glm::vec3(0.0f, 1.0f, 0.0f));
  child.parent = 1;
  scene::Instance hidden = MakeInstance(3, 0);
  hidden.visible = false;
  scene::Instance below_hidden = MakeInstance(4, 0);
  below_hidden.parent = 3;
  scene::Instance orphan = MakeInstance(5, 0);
  orphan.parent = 99;
  scene::Instance loop_a = MakeInstance(6, 0);
  loop_a.parent = 7;
  scene::Instance loop_b = MakeInstance(7, 0);
  loop_b.parent = 6;
  scene.instances = {child,
                     MakeInstance(1, 0, glm::vec3(5.0f, 0.0f, 0.0f)),
                     hidden,
                     below_hidden,
                     orphan,
                     loop_a,
                     loop_b,
                     MakeInstance(8, 1),
                     MakeInstance(9, 1),
                     MakeInstance(10, 4)};

  const PlacementPlan plan = ResolvePlacements(scene, store);
  std::vector<scene::InstanceId> order;
  for (const PlannedInstance& planned : plan.instances) {
    order.push_back(planned.id);
  }
  CHECK(order == std::vector<scene::InstanceId>{1, 2, 8, 9, 10});
  REQUIRE(plan.placements.size() == 2);
  CHECK(plan.placements[0].owner == 1);
  CHECK(plan.placements[1].owner == 2);
  const glm::vec3 child_origin = glm::vec3(plan.placements[1].world[3]);
  CHECK(child_origin == glm::vec3(5.0f, 1.0f, 0.0f));
  CHECK(plan.instances[1].scene_index == 0);
  // Group-like instances without a loadable prefab are kept, unplaced.
  CHECK(plan.instances[2].first_placement == -1);
  CHECK(plan.instances[2].placement_count == 0);

  REQUIRE(plan.issues.size() == 4);
  CHECK(plan.issues[0].instance == 5);
  CHECK(plan.issues[0].message ==
        "broken parent chain (missing parent or a cycle)");
  CHECK(plan.issues[1].instance == 7);
  CHECK(plan.issues[1].message ==
        "broken parent chain (missing parent or a cycle)");
  // The missing prefab is reported once, for its first user.
  CHECK(plan.issues[2].instance == 8);
  CHECK(plan.issues[2].message.starts_with("cannot load prefab " +
                                           dir.Child("missing.pbo")));
  CHECK(plan.issues[3].instance == 10);
  CHECK(plan.issues[3].message == "prefab index 4 out of range");
  // Issues are attributed in build order.
  CHECK(plan.instances[0].issue_end == 2);
  CHECK(plan.instances[2].issue_end == 3);
  CHECK(plan.instances[3].issue_end == 3);
  CHECK(plan.instances[4].issue_end == 4);
  REQUIRE(plan.scene_prefabs.size() == 2);
  REQUIRE(plan.scene_prefabs[0].has_value());
  CHECK(*plan.scene_prefabs[0] == store.Find(box));
  REQUIRE(plan.scene_prefabs[1].has_value());
  CHECK_FALSE(plan.scene_prefabs[1]->has_value());
}

}  // namespace
}  // namespace blue::world
