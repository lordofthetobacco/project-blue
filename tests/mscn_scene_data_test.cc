#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "scene/scene_data.h"

namespace blue::scene {
namespace {

Instance MakeInstance(std::string name, int prefab,
                      InstanceId parent = kNoInstance) {
  Instance instance;
  instance.name = std::move(name);
  instance.prefab = prefab;
  instance.parent = parent;
  return instance;
}

MaterialOverride ReplaceWith(std::string material, int source_prefab,
                             std::string source_material) {
  MaterialOverride override_entry;
  override_entry.material_name = std::move(material);
  override_entry.source_prefab = source_prefab;
  override_entry.source_material = std::move(source_material);
  return override_entry;
}

}  // namespace

TEST_CASE("AddPrefab deduplicates normalized paths") {
  SceneData scene;
  CHECK(AddPrefab(scene, "C:/a/b.pbo") == 0);
  CHECK(AddPrefab(scene, "C:/a/c.pbo") == 1);
  CHECK(AddPrefab(scene, "C:\\a\\.\\b.pbo") == 0);
  CHECK(scene.prefabs == std::vector<std::string>{"C:/a/b.pbo", "C:/a/c.pbo"});
}

TEST_CASE("AddInstance assigns increasing ids and rejects bad references") {
  SceneData scene;
  const int prefab = AddPrefab(scene, "C:/p.pbo");
  const InstanceId a = AddInstance(scene, MakeInstance("a", prefab));
  const InstanceId b = AddInstance(scene, MakeInstance("b", kNoPrefab, a));
  CHECK(a == 1);
  CHECK(b == 2);
  CHECK(scene.next_instance_id == 3);
  CHECK(AddInstance(scene, MakeInstance("bad", 5)) == kNoInstance);
  CHECK(AddInstance(scene, MakeInstance("bad", prefab, 99)) == kNoInstance);
  CHECK(scene.instances.size() == 2);
  CHECK_FALSE(ValidateSceneData(scene).has_value());
}

TEST_CASE("RemoveInstance removes the subtree and compacts the prefabs") {
  SceneData scene;
  const int house = AddPrefab(scene, "C:/house.pbo");
  const int rock = AddPrefab(scene, "C:/rock.pbo");
  const int library = AddPrefab(scene, "C:/library.pbo");
  const int tree = AddPrefab(scene, "C:/tree.pbo");
  const InstanceId group = AddInstance(scene, MakeInstance("g", kNoPrefab));
  const InstanceId child = AddInstance(scene, MakeInstance("h", house, group));
  AddInstance(scene, MakeInstance("grandchild", rock, child));
  Instance with_override = MakeInstance("t", tree);
  with_override.materials.push_back(ReplaceWith("bark", library, "steel"));
  const InstanceId kept = AddInstance(scene, with_override);

  CHECK(RemoveInstance(scene, group));
  CHECK_FALSE(RemoveInstance(scene, group));
  REQUIRE(scene.instances.size() == 1);
  CHECK(scene.instances[0].id == kept);
  // house and rock are gone; library (override source) and tree stay, in
  // their original order.
  CHECK(scene.prefabs ==
        std::vector<std::string>{"C:/library.pbo", "C:/tree.pbo"});
  CHECK(scene.instances[0].prefab == 1);
  CHECK(scene.instances[0].materials[0].source_prefab == 0);
  CHECK_FALSE(ValidateSceneData(scene).has_value());

  // Ids are never reused.
  CHECK(AddInstance(scene, MakeInstance("new", 0)) == 5);
}

TEST_CASE("DuplicateInstance copies the subtree with fresh ids") {
  SceneData scene;
  const int prefab = AddPrefab(scene, "C:/p.pbo");
  const InstanceId root = AddInstance(scene, MakeInstance("root", prefab));
  const InstanceId group = AddInstance(scene, MakeInstance("g", kNoPrefab));
  const InstanceId child = AddInstance(scene, MakeInstance("c", prefab, group));
  AddInstance(scene, MakeInstance("cc", prefab, child));

  const InstanceId copy = DuplicateInstance(scene, group);
  CHECK(copy == 5);
  REQUIRE(scene.instances.size() == 7);
  const Instance* copied_group = FindInstance(scene, copy);
  REQUIRE(copied_group != nullptr);
  CHECK(copied_group->name == "g");
  CHECK(copied_group->parent == kNoInstance);
  CHECK(FindInstance(scene, 6)->parent == copy);
  CHECK(FindInstance(scene, 6)->name == "c");
  CHECK(FindInstance(scene, 7)->parent == 6);
  CHECK(scene.next_instance_id == 8);
  CHECK(DuplicateInstance(scene, 42) == kNoInstance);
  CHECK(FindInstance(scene, root)->parent == kNoInstance);
  CHECK_FALSE(ValidateSceneData(scene).has_value());
}

TEST_CASE("WorldMatrix composes the parent chain") {
  SceneData scene;
  Instance parent = MakeInstance("p", kNoPrefab);
  parent.transform.position = {1.0f, 0.0f, 0.0f};
  parent.transform.scale = glm::vec3(2.0f);
  const InstanceId parent_id = AddInstance(scene, parent);
  Instance child = MakeInstance("c", kNoPrefab, parent_id);
  child.transform.position = {0.0f, 1.0f, 0.0f};
  const InstanceId child_id = AddInstance(scene, child);

  const std::optional<glm::mat4> world = WorldMatrix(scene, child_id);
  REQUIRE(world.has_value());
  const glm::vec4 origin = *world * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  CHECK(origin.x == doctest::Approx(1.0f));
  CHECK(origin.y == doctest::Approx(2.0f));
  CHECK_FALSE(WorldMatrix(scene, 77).has_value());

  // A cycle (only possible through direct edits) is detected.
  FindInstance(scene, parent_id)->parent = child_id;
  CHECK_FALSE(WorldMatrix(scene, child_id).has_value());
  CHECK(ValidateSceneData(scene).has_value());
}

TEST_CASE("FindMaterialOverride matches by material name") {
  SceneData scene;
  const int prefab = AddPrefab(scene, "C:/p.pbo");
  Instance instance = MakeInstance("i", prefab);
  MaterialOverride tweak;
  tweak.material_name = "paint";
  tweak.roughness = 0.2f;
  instance.materials.push_back(tweak);
  const InstanceId id = AddInstance(scene, instance);

  const MaterialOverride* found = FindMaterialOverride(scene, id, "paint");
  REQUIRE(found != nullptr);
  CHECK(found->roughness == 0.2f);
  CHECK(FindMaterialOverride(scene, id, "metal") == nullptr);
  CHECK(FindMaterialOverride(scene, 99, "paint") == nullptr);
}

TEST_CASE("ValidateAgainstPrefabs reports unknown material names") {
  SceneData scene;
  const int gun = AddPrefab(scene, "C:/gun.pbo");
  const int library = AddPrefab(scene, "C:/library.pbo");
  Instance instance = MakeInstance("gun", gun);
  instance.materials.push_back(ReplaceWith("frame", library, "steel"));
  instance.materials.push_back(ReplaceWith("missing", library, "gold"));
  const InstanceId id = AddInstance(scene, instance);

  const std::vector<std::vector<std::string>> names = {{"frame", "slide"},
                                                       {"steel"}};
  const std::vector<SceneIssue> issues = ValidateAgainstPrefabs(scene, names);
  REQUIRE(issues.size() == 2);
  CHECK(issues[0].instance == id);
  CHECK(issues[0].message.find("missing") != std::string::npos);
  CHECK(issues[1].message.find("gold") != std::string::npos);

  // Prefabs without name lists are not checked.
  CHECK(ValidateAgainstPrefabs(scene, {}).empty());
}

TEST_CASE("ValidateSceneData rejects inconsistent scenes") {
  SceneData scene;
  const int prefab = AddPrefab(scene, "C:/p.pbo");
  AddInstance(scene, MakeInstance("a", prefab));
  AddInstance(scene, MakeInstance("b", prefab));
  REQUIRE_FALSE(ValidateSceneData(scene).has_value());

  SUBCASE("duplicate id") { scene.instances[1].id = scene.instances[0].id; }
  SUBCASE("id at or above next_instance_id") { scene.instances[1].id = 3; }
  SUBCASE("zero id") { scene.instances[1].id = kNoInstance; }
  SUBCASE("bad prefab index") { scene.instances[0].prefab = 1; }
  SUBCASE("missing parent") { scene.instances[0].parent = 9; }
  SUBCASE("self parent") { scene.instances[0].parent = 1; }
  SUBCASE("duplicate prefab path") { scene.prefabs.push_back("C:/p.pbo"); }
  SUBCASE("non-finite transform") {
    scene.instances[0].transform.position.x =
        std::numeric_limits<float>::infinity();
  }
  SUBCASE("override without name") {
    scene.instances[0].materials.push_back(MaterialOverride{});
  }
  SUBCASE("override named twice") {
    MaterialOverride tweak;
    tweak.material_name = "m";
    scene.instances[0].materials = {tweak, tweak};
  }
  SUBCASE("source material without prefab") {
    scene.instances[0].materials.push_back(
        ReplaceWith("m", kNoPrefab, "steel"));
  }
  SUBCASE("roughness out of range") {
    MaterialOverride tweak;
    tweak.material_name = "m";
    tweak.roughness = 1.5f;
    scene.instances[0].materials.push_back(tweak);
  }
  SUBCASE("zero mass") {
    scene.instances[0].physics = PhysicsOverride{.mass = 0.0f};
  }
  SUBCASE("negative light range") {
    scene.point_lights.push_back(ScenePointLight{.range = -1.0f});
  }
  SUBCASE("invalid sun") { scene.sun.direction = glm::vec3(0.0f); }
  CHECK(ValidateSceneData(scene).has_value());
}

namespace {

void CheckSameWorld(const SceneData& scene, InstanceId id,
                    const glm::mat4& expected) {
  const std::optional<glm::mat4> world = WorldMatrix(scene, id);
  REQUIRE(world.has_value());
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      CHECK((*world)[column][row] ==
            doctest::Approx(expected[column][row]).epsilon(1e-4));
    }
  }
}

Instance MakePlaced(std::string name, const glm::vec3& position,
                    float yaw_radians, float scale) {
  Instance instance = MakeInstance(std::move(name), kNoPrefab);
  instance.transform.position = position;
  instance.transform.rotation =
      glm::angleAxis(yaw_radians, glm::vec3(0.0f, 1.0f, 0.0f));
  instance.transform.scale = glm::vec3(scale);
  return instance;
}

}  // namespace

TEST_CASE("TransformFromMatrix inverts Transform::Matrix") {
  Transform transform;
  transform.position = {1.0f, -2.0f, 3.0f};
  transform.rotation = glm::normalize(glm::quat(0.8f, 0.2f, -0.4f, 0.1f));
  transform.scale = {2.0f, 0.5f, 3.0f};
  const Transform back = TransformFromMatrix(transform.Matrix());
  const glm::mat4 a = transform.Matrix();
  const glm::mat4 b = back.Matrix();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      CHECK(b[column][row] == doctest::Approx(a[column][row]).epsilon(1e-5));
    }
  }
  transform.scale.x = -1.0f;  // Mirrored.
  const glm::mat4 mirrored = transform.Matrix();
  const glm::mat4 rebuilt = TransformFromMatrix(mirrored).Matrix();
  CHECK(rebuilt[0][0] == doctest::Approx(mirrored[0][0]).epsilon(1e-5));
  CHECK(rebuilt[2][1] == doctest::Approx(mirrored[2][1]).epsilon(1e-5));
}

TEST_CASE("ReparentInstance keeps the world transform and rejects cycles") {
  SceneData scene;
  const InstanceId a =
      AddInstance(scene, MakePlaced("a", {5.0f, 0.0f, 0.0f}, 0.7f, 2.0f));
  const InstanceId b =
      AddInstance(scene, MakePlaced("b", {0.0f, 1.0f, 2.0f}, -0.3f, 1.0f));
  const glm::mat4 b_world = *WorldMatrix(scene, b);

  REQUIRE(ReparentInstance(scene, b, a, /*keep_world_transform=*/true));
  CHECK(FindInstance(scene, b)->parent == a);
  CheckSameWorld(scene, b, b_world);

  CHECK_FALSE(ReparentInstance(scene, a, b, true));  // b is a descendant.
  CHECK_FALSE(ReparentInstance(scene, a, a, true));
  CHECK_FALSE(ReparentInstance(scene, a, 99, true));
  CHECK_FALSE(ReparentInstance(scene, 99, a, true));

  REQUIRE(ReparentInstance(scene, b, kNoInstance, true));
  CHECK(FindInstance(scene, b)->parent == kNoInstance);
  CheckSameWorld(scene, b, b_world);

  // Without keep_world_transform the local transform is kept as is.
  const Transform local = FindInstance(scene, b)->transform;
  REQUIRE(ReparentInstance(scene, b, a, /*keep_world_transform=*/false));
  CHECK(FindInstance(scene, b)->transform.position == local.position);
  CHECK_FALSE(ValidateSceneData(scene).has_value());
}

TEST_CASE("GroupInstances and UngroupInstance keep world transforms") {
  SceneData scene;
  const InstanceId parent =
      AddInstance(scene, MakePlaced("parent", {10.0f, 0.0f, 0.0f}, 1.0f, 1.0f));
  Instance a = MakePlaced("a", {2.0f, 0.0f, 0.0f}, 0.2f, 1.0f);
  a.parent = parent;
  Instance b = MakePlaced("b", {4.0f, 0.0f, 2.0f}, 0.0f, 1.5f);
  b.parent = parent;
  const InstanceId a_id = AddInstance(scene, a);
  const InstanceId b_id = AddInstance(scene, b);
  Instance nested = MakePlaced("nested", {0.0f, 1.0f, 0.0f}, 0.0f, 1.0f);
  nested.parent = a_id;
  const InstanceId nested_id = AddInstance(scene, nested);
  const glm::mat4 a_world = *WorldMatrix(scene, a_id);
  const glm::mat4 b_world = *WorldMatrix(scene, b_id);
  const glm::mat4 nested_world = *WorldMatrix(scene, nested_id);

  const std::vector<InstanceId> ids = {a_id, b_id, nested_id, 99};
  const InstanceId group = GroupInstances(scene, ids, "group");
  REQUIRE(group != kNoInstance);
  const Instance* group_instance = FindInstance(scene, group);
  CHECK(group_instance->parent == parent);  // Shared parent kept.
  CHECK(group_instance->prefab == kNoPrefab);
  const glm::vec3 centroid =
      0.5f * (glm::vec3(a_world[3]) + glm::vec3(b_world[3]));
  const glm::vec3 group_position(*WorldMatrix(scene, group) *
                                 glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  CHECK(group_position.x == doctest::Approx(centroid.x));
  CHECK(group_position.z == doctest::Approx(centroid.z));
  CHECK(FindInstance(scene, a_id)->parent == group);
  CHECK(FindInstance(scene, b_id)->parent == group);
  CHECK(FindInstance(scene, nested_id)->parent == a_id);  // Moved with a.
  CheckSameWorld(scene, a_id, a_world);
  CheckSameWorld(scene, b_id, b_world);
  CheckSameWorld(scene, nested_id, nested_world);
  CHECK(ChildrenOf(scene, group) == std::vector<InstanceId>{a_id, b_id});

  REQUIRE(UngroupInstance(scene, group));
  CHECK(FindInstance(scene, group) == nullptr);
  CHECK(FindInstance(scene, a_id)->parent == parent);
  CheckSameWorld(scene, a_id, a_world);
  CheckSameWorld(scene, b_id, b_world);
  CHECK_FALSE(UngroupInstance(scene, group));
  CHECK(GroupInstances(scene, std::vector<InstanceId>{77}, "x") == kNoInstance);
  CHECK_FALSE(ValidateSceneData(scene).has_value());
}

TEST_CASE("Sun angles convert both ways") {
  const SunLight sun;
  const SunAngles angles = SunAnglesFromDirection(sun.direction);
  CHECK(angles.elevation > 0.0f);  // The default sun is above the horizon.
  const glm::vec3 back = SunDirectionFromAngles(angles);
  CHECK(back.x == doctest::Approx(sun.direction.x).epsilon(1e-5));
  CHECK(back.y == doctest::Approx(sun.direction.y).epsilon(1e-5));
  CHECK(back.z == doctest::Approx(sun.direction.z).epsilon(1e-5));

  // Straight overhead: light travels down.
  const glm::vec3 overhead = SunDirectionFromAngles({1.5707964f, 0.0f});
  CHECK(overhead.y == doctest::Approx(-1.0f));
  // On the horizon towards +X: light travels along -X.
  const glm::vec3 east = SunDirectionFromAngles({0.0f, 1.5707964f});
  CHECK(east.x == doctest::Approx(-1.0f));
}

}  // namespace blue::scene
