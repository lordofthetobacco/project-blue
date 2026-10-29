#include <doctest/doctest.h>

#include <string>

#include "asset/mesh_data.h"
#include "asset/model_importer.h"
#include "core/path.h"
#include "platform/file_system.h"
#include "scene/placement.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "tests/scratch_directory.h"

namespace blue::scene {

namespace {

constexpr size_t kTeapotTriangles = 6320;

}  // namespace

TEST_CASE("an imported, placed model survives a scene save and load") {
  const std::string teapot_path =
      core::JoinPath(BLUE_SAMPLES_DIR, "teapot.obj");
  const auto imported = asset::ImportModel(teapot_path);
  REQUIRE(imported.ok());

  const asset::MeshData plane = asset::MakePlane(12.0f, glm::vec3(0.1f));
  Scene scene;
  scene.entities.push_back(
      {.name = "ground", .mesh_path = "builtin:plane", .transform = {}});
  scene.entities.push_back({
      .name = "teapot",
      .mesh_path = teapot_path,
      .transform =
          FitOnGround(imported.value().bounds.min, imported.value().bounds.max,
                      2.0f, {1.5f, plane.bounds.min.y, -0.5f}),
  });

  scene.entities[0].body = RigidBody{.motion = MotionType::kStatic,
                                     .shape = ShapeType::kBox,
                                     .mass = 1.0f,
                                     .friction = 0.9f,
                                     .restitution = 0.0f};
  scene.entities[1].body = RigidBody{.motion = MotionType::kDynamic,
                                     .shape = ShapeType::kConvexHull,
                                     .mass = 0.75f,
                                     .friction = 0.4f,
                                     .restitution = 0.3f};

  const testing::ScratchDirectory dir("roundtrip");
  const std::string scene_path = dir.Child("scenes/roundtrip.bscn");
  REQUIRE(SaveScene(scene, scene_path).ok());

  const auto loaded = LoadScene(scene_path);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().entities.size() == scene.entities.size());
  CHECK(loaded.value().gravity == scene.gravity);
  for (size_t i = 0; i < scene.entities.size(); ++i) {
    REQUIRE(loaded.value().entities[i].body.has_value());
    const RigidBody& expected = *scene.entities[i].body;
    const RigidBody& actual = *loaded.value().entities[i].body;
    CHECK((actual.motion == expected.motion));
    CHECK((actual.shape == expected.shape));
    CHECK(actual.mass == expected.mass);
    CHECK(actual.friction == expected.friction);
    CHECK(actual.restitution == expected.restitution);
  }

  const Entity& teapot = loaded.value().entities[1];
  CHECK(loaded.value().entities[0].mesh_path == "builtin:plane");
  REQUIRE(platform::IsRegularFile(teapot.mesh_path));

  const auto reimported = asset::ImportModel(teapot.mesh_path);
  REQUIRE(reimported.ok());
  CHECK(reimported.value().indices.size() / 3 == kTeapotTriangles);

  for (size_t i = 0; i < scene.entities.size(); ++i) {
    const Transform& expected = scene.entities[i].transform;
    const Transform& actual = loaded.value().entities[i].transform;
    CHECK(actual.position == expected.position);
    CHECK(actual.rotation == expected.rotation);
    CHECK(actual.scale == expected.scale);
  }
}

}  // namespace blue::scene
