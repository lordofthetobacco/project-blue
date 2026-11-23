#include "cook/scene_edits.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/ext/quaternion_trigonometric.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "core/path.h"
#include "core/result.h"
#include "platform/file_system.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"
#include "tests/scratch_directory.h"

namespace blue::cook {
namespace {

using Kind = SceneEdit::Kind;

SceneEdit Parse(std::string_view option, std::string_view value) {
  core::Result<SceneEdit, std::string> edit = ParseSceneEdit(option, value);
  REQUIRE(edit.ok());
  return std::move(edit).value();
}

bool ParseFails(std::string_view option, std::string_view value) {
  return !ParseSceneEdit(option, value).ok();
}

// A small scene: a group with a child crate, a target at the root, a lamp
// and a non-default player start.
scene::SceneData MakeScene(const testing::ScratchDirectory& dir) {
  scene::SceneData scene;
  const int crate = scene::AddPrefab(scene, dir.Child("assets/crate.pbo"));
  const int target = scene::AddPrefab(scene, dir.Child("assets/target.pbo"));
  const scene::InstanceId group =
      scene::AddInstance(scene, {.name = "props", .transform = {}});
  scene::AddInstance(scene, {.name = "crate",
                             .prefab = crate,
                             .parent = group,
                             .transform = {.position = {1.0f, 0.0f, 2.0f}}});
  scene::AddInstance(scene, {.name = "target", .prefab = target});
  scene.point_lights.push_back({.name = "lamp", .position = {0, 3, 0}});
  scene.player_start = {.position = {0.0f, 1.7f, 4.0f}, .yaw_radians = 0.5f};
  return scene;
}

void WriteDummyPrefab(const std::string& path) {
  const std::byte byte{0};
  REQUIRE(platform::WriteFileAtomic(path, std::span(&byte, 1)).ok());
}

TEST_CASE("Scene edit options parse") {
  CHECK(IsSceneEditOption("--add-instance"));
  CHECK(IsSceneEditOption("--remove-instance"));
  CHECK_FALSE(IsSceneEditOption("--tag"));

  const SceneEdit add =
      Parse("--add-instance", "Glock17_Gen5=C:/a@b/glock.pbo@1.918,0,-1.473");
  CHECK(add.kind == Kind::kAddInstance);
  CHECK(add.name == "Glock17_Gen5");
  // The path ends at the last '@'.
  CHECK(add.prefab_path == "C:/a@b/glock.pbo");
  CHECK(add.position == glm::vec3(1.918f, 0.0f, -1.473f));
  CHECK(add.yaw_degrees == 0.0f);
  // The yaw follows the last '/' after the '@'.
  CHECK(Parse("--add-instance", "g=w/glock.pbo@1,2,3/90").yaw_degrees == 90.0f);

  const SceneEdit remove = Parse("--remove-instance", "crate");
  CHECK(remove.kind == Kind::kRemoveInstance);
  CHECK(remove.name == "crate");

  CHECK(ParseFails("--add-instance", "glock.pbo@1,2,3"));      // no name
  CHECK(ParseFails("--add-instance", "=glock.pbo@1,2,3"));     // empty name
  CHECK(ParseFails("--add-instance", "g=@1,2,3"));             // no prefab
  CHECK(ParseFails("--add-instance", "g=glock.pbo"));          // no position
  CHECK(ParseFails("--add-instance", "g=glock.pbo@1,2"));      // short vec
  CHECK(ParseFails("--add-instance", "g=glock.pbo@1,2,3/x"));  // bad yaw
  CHECK(ParseFails("--add-instance", "g=glock.pbo@1,2,nan"));
  CHECK(ParseFails("--remove-instance", ""));
  CHECK(ParseFails("--tag", "a=b"));
}

TEST_CASE("Adding an instance places it at the root with yaw and scale 1") {
  testing::ScratchDirectory dir("scene_edit_add");
  scene::SceneData scene = MakeScene(dir);
  const scene::SceneData before = scene;
  const std::string glock = dir.Child("assets/weapons/glock.pbo");
  REQUIRE(ApplySceneEdit(
              scene, Parse("--add-instance", "pistol=" + glock + "@1,0,-2/90"))
              .ok());
  REQUIRE(scene.instances.size() == before.instances.size() + 1);
  REQUIRE(scene.prefabs.size() == before.prefabs.size() + 1);
  const scene::Instance& added = scene.instances.back();
  CHECK(added.name == "pistol");
  CHECK(added.parent == scene::kNoInstance);
  CHECK(added.id == before.next_instance_id);
  CHECK(scene.prefabs[static_cast<size_t>(added.prefab)] ==
        core::NormalizePath(glock));
  CHECK(added.transform.position == glm::vec3(1.0f, 0.0f, -2.0f));
  CHECK(added.transform.scale == glm::vec3(1.0f));
  const glm::quat expected =
      glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  CHECK(std::abs(glm::dot(added.transform.rotation, expected)) ==
        doctest::Approx(1.0f));

  // A second instance of a known prefab reuses its entry.
  const int prefab = added.prefab;
  REQUIRE(ApplySceneEdit(scene,
                         Parse("--add-instance", "spare=" + glock + "@0,0,0"))
              .ok());
  CHECK(scene.prefabs.size() == before.prefabs.size() + 1);
  CHECK(scene.instances.back().prefab == prefab);
}

TEST_CASE("Scene edits refuse ambiguous or missing names") {
  testing::ScratchDirectory dir("scene_edit_names");
  scene::SceneData scene = MakeScene(dir);
  const scene::SceneData before = scene;
  const std::string crate = dir.Child("assets/crate.pbo");

  // Duplicate name on add (also of a nested instance).
  CHECK_FALSE(ApplySceneEdit(
                  scene, Parse("--add-instance", "crate=" + crate + "@0,0,0"))
                  .ok());
  CHECK(scene == before);
  CHECK_FALSE(ApplySceneEdit(scene, Parse("--remove-instance", "nope")).ok());
  CHECK(scene == before);

  // Two instances of one name (made in the editor): remove refuses.
  scene::AddInstance(scene, {.name = "target", .prefab = 1});
  const scene::SceneData twice = scene;
  core::Result<void, std::string> removed =
      ApplySceneEdit(scene, Parse("--remove-instance", "target"));
  REQUIRE_FALSE(removed.ok());
  CHECK(removed.error().find("2 instances") != std::string::npos);
  CHECK(scene == twice);
}

TEST_CASE("Removing an instance removes its subtree and unused prefabs") {
  testing::ScratchDirectory dir("scene_edit_remove");
  scene::SceneData scene = MakeScene(dir);
  REQUIRE(ApplySceneEdit(scene, Parse("--remove-instance", "props")).ok());
  REQUIRE(scene.instances.size() == 1);
  CHECK(scene.instances[0].name == "target");
  // crate.pbo is no longer used; target.pbo is renumbered to 0.
  REQUIRE(scene.prefabs.size() == 1);
  CHECK(scene.instances[0].prefab == 0);
  CHECK(scene.point_lights.size() == 1);
}

TEST_CASE("ApplySceneEdits is all or nothing") {
  testing::ScratchDirectory dir("scene_edit_batch");
  const scene::SceneData scene = MakeScene(dir);
  const std::vector<SceneEdit> edits{
      Parse("--remove-instance", "target"),
      Parse("--remove-instance", "target"),
  };
  core::Result<scene::SceneData, std::string> edited =
      ApplySceneEdits(scene, edits);
  REQUIRE_FALSE(edited.ok());
  CHECK(edited.error().starts_with("edit 2:"));
}

TEST_CASE("EditSceneFile round-trips and keeps everything else") {
  testing::ScratchDirectory dir("scene_edit_file");
  const std::string path = dir.Child("assets/range.mscn");
  const scene::SceneData original = MakeScene(dir);
  REQUIRE(scene::SaveMscn(original, path).ok());
  const std::string glock = dir.Child("assets/weapons/glock17/Glock17.pbo");
  WriteDummyPrefab(glock);

  const std::vector<SceneEdit> edits{
      Parse("--add-instance", "Glock17_Gen5=" + glock + "@1.918,0,-1.473"),
  };
  REQUIRE(EditSceneFile(path, path, edits).ok());
  core::Result<scene::SceneData, scene::MscnError> loaded =
      scene::LoadMscn(path);
  REQUIRE(loaded.ok());
  const scene::SceneData& edited = loaded.value();

  // The edited scene equals the original plus the instance (the original
  // loaded back first, so paths compare in their resolved form).
  core::Result<scene::SceneData, scene::MscnError> reloaded_original =
      scene::DecodeMscn(scene::EncodeMscn(original, path).value(), path);
  REQUIRE(reloaded_original.ok());
  core::Result<scene::SceneData, std::string> expected =
      ApplySceneEdits(reloaded_original.value(), edits);
  REQUIRE(expected.ok());
  CHECK(edited == expected.value());
  // The prefab path is stored relative to the scene, like the editor's.
  const scene::Instance& added = edited.instances.back();
  CHECK(added.name == "Glock17_Gen5");
  CHECK(edited.prefabs[static_cast<size_t>(added.prefab)] ==
        core::NormalizePath(glock));

  // Removing it again gives the original back.
  REQUIRE(EditSceneFile(path, path,
                        std::vector<SceneEdit>{
                            Parse("--remove-instance", "Glock17_Gen5")})
              .ok());
  core::Result<scene::SceneData, scene::MscnError> restored =
      scene::LoadMscn(path);
  REQUIRE(restored.ok());
  CHECK(restored.value().instances == reloaded_original.value().instances);
  CHECK(restored.value().prefabs == reloaded_original.value().prefabs);
}

TEST_CASE("A failed scene edit leaves the file untouched") {
  testing::ScratchDirectory dir("scene_edit_fail");
  const std::string path = dir.Child("range.mscn");
  REQUIRE(scene::SaveMscn(MakeScene(dir), path).ok());
  const auto before = platform::ReadFile(path, 1024 * 1024);
  REQUIRE(before.ok());
  const std::string glock = dir.Child("glock.pbo");
  WriteDummyPrefab(glock);

  // The second edit fails: nothing is written.
  CHECK_FALSE(
      EditSceneFile(path, path,
                    std::vector<SceneEdit>{
                        Parse("--add-instance", "g=" + glock + "@0,0,0"),
                        Parse("--remove-instance", "nope")})
          .ok());
  // A missing prefab file is refused.
  CHECK_FALSE(EditSceneFile(path, path,
                            std::vector<SceneEdit>{Parse(
                                "--add-instance",
                                "g=" + dir.Child("missing.pbo") + "@0,0,0")})
                  .ok());
  const auto after = platform::ReadFile(path, 1024 * 1024);
  REQUIRE(after.ok());
  CHECK(before.value() == after.value());
  CHECK_FALSE(EditSceneFile(
                  dir.Child("missing.mscn"), path,
                  std::vector<SceneEdit>{Parse("--remove-instance", "target")})
                  .ok());
}

}  // namespace
}  // namespace blue::cook
