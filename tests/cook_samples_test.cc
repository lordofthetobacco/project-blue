// Checks the sample assets `blue-pack samples` cooked into the build's
// assets directory (target blue_sample_assets runs before the tests): what
// the game and the editor rely on.

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "core/path.h"
#include "physics/physics_runtime.h"
#include "physics/shape_cooking.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "scene/mscn_serializer.h"
#include "scene/scene_data.h"

namespace blue {
namespace {

std::string Asset(std::string_view name) {
  return core::JoinPath(BLUE_ASSETS_DIR, name);
}

prefab::Prefab Load(std::string_view name) {
  CAPTURE(name);
  auto loaded = prefab::LoadPrefab(Asset(name));
  REQUIRE(loaded.ok());
  CHECK(prefab::ValidatePrefab(loaded.value()) == std::nullopt);
  return std::move(loaded).value();
}

const prefab::Socket& Socket(const prefab::Prefab& prefab,
                             std::string_view name) {
  CAPTURE(name);
  const int index = prefab::FindSocket(prefab, name);
  REQUIRE(index >= 0);
  return prefab.sockets[static_cast<size_t>(index)];
}

bool Near(glm::vec3 a, glm::vec3 b, float tolerance = 1.0e-4f) {
  return glm::length(a - b) <= tolerance;
}

TEST_CASE("The cooked M16 is in metres with its four sockets") {
  const prefab::Prefab rifle = Load("m16.pbo");
  CHECK(rifle.meshes.size() == 1);
  CHECK(rifle.materials.size() == 4);
  CHECK(rifle.physics.kind == prefab::ColliderKind::kNone);
  // 14.92 model units long x 0.0771.
  CHECK(rifle.bounds.size().z == doctest::Approx(1.150f).epsilon(0.01));

  constexpr float kScale = 0.0771f;
  CHECK(Near(Socket(rifle, "muzzle").local.translation,
             glm::vec3(0.0f, 0.598f, 8.995f) * kScale));
  CHECK(Near(Socket(rifle, "grip").local.translation,
             glm::vec3(0.0f, -0.75f, -2.0f) * kScale));
  const prefab::Socket& sight = Socket(rifle, "sight");
  CHECK(
      Near(sight.local.translation, glm::vec3(0.0f, 1.44f, -1.505f) * kScale));
  CHECK(Near(sight.local.rotation * glm::vec3(0, 0, 1), {0, 0, 1}));
  const prefab::Socket& lens = Socket(rifle, "lens");
  CHECK(Near(lens.local.translation, glm::vec3(0.0f, 1.354f, -1.27f) * kScale));
  CHECK(lens.local.scale.x == doctest::Approx(0.11f * kScale));
  for (const prefab::Socket& socket : rifle.sockets) {
    CHECK(socket.node == -1);
  }
}

TEST_CASE("The cooked Glock has one fire clip with its events") {
  const prefab::Prefab pistol = Load("glock17.pbo");
  CHECK(animation::FindNode(pistol.hierarchy, "Glock_parts_ORIGINAL_combined") <
        0);
  REQUIRE(!pistol.hierarchy.nodes.empty());
  CHECK(pistol.hierarchy.nodes[0].name == "glock17");
  CHECK(pistol.hierarchy.nodes[0].rest.scale.x == doctest::Approx(0.0842f));
  CHECK(pistol.meshes.size() == 6);
  CHECK(pistol.materials.size() == 2);
  CHECK(pistol.textures.size() == 6);
  for (const prefab::Texture& texture : pistol.textures) {
    CHECK(prefab::IsBlockCompressed(texture.format));
    CHECK(texture.mip_count() ==
          prefab::FullMipCount(texture.width, texture.height));
  }
  // 2.37 model units long x 0.0842 = 0.2 m.
  CHECK(pistol.bounds.size().x == doctest::Approx(0.2f).epsilon(0.02));

  REQUIRE(pistol.clips.size() == 1);
  const animation::Clip& fire = pistol.clips[0];
  CHECK(fire.name == "fire");
  CHECK(!fire.loop);
  CHECK(fire.channels.size() == 6);
  const animation::ClipEvent* shot = animation::FindEvent(fire, "shot");
  const animation::ClipEvent* lock = animation::FindEvent(fire, "slide_lock");
  REQUIRE(shot != nullptr);
  REQUIRE(lock != nullptr);
  CHECK(shot->time == doctest::Approx(0.333f));
  CHECK(lock->time == doctest::Approx(0.4f));

  constexpr float kScale = 0.0842f;
  CHECK(Near(Socket(pistol, "muzzle").local.translation,
             glm::vec3(-1.211f, 0.626f, 0.0f) * kScale));
  const prefab::Socket& sight = Socket(pistol, "sight");
  CHECK(Near(sight.local.rotation * glm::vec3(0, 0, 1),
             glm::normalize(glm::vec3(-0.999990f, 0.004527f, 0.0f))));
  CHECK(prefab::FindSocket(pistol, "grip") >= 0);
  CHECK(prefab::FindSocket(pistol, "lens") < 0);
}

TEST_CASE("The cooked static props have the expected bodies") {
  physics::PhysicsRuntime runtime;
  struct Expected {
    const char* file;
    prefab::ColliderKind kind;
    bool dynamic;
  };
  for (const Expected& expected :
       {Expected{"house.pbo", prefab::ColliderKind::kTriangleMesh, false},
        Expected{"ground.pbo", prefab::ColliderKind::kBox, false},
        Expected{"target.pbo", prefab::ColliderKind::kBox, false},
        Expected{"crate.pbo", prefab::ColliderKind::kBox, true},
        Expected{"sphere.pbo", prefab::ColliderKind::kSphere, false}}) {
    CAPTURE(expected.file);
    const prefab::Prefab prop = Load(expected.file);
    CHECK(prop.physics.kind == expected.kind);
    CHECK(prop.physics.dynamic == expected.dynamic);
    CHECK(physics::CookedShape::Restore(prop.physics.cooked_shape).ok());
    CHECK(prop.meshes.size() == 1);
  }

  const prefab::Prefab ground = Load("ground.pbo");
  CHECK(ground.bounds.size().x == doctest::Approx(48.0f));
  CHECK(ground.bounds.size().z == doctest::Approx(724.0f));
  REQUIRE(ground.materials.size() == 1);
  CHECK(ground.materials[0].uv_scale.x == doctest::Approx(0.5f));
  CHECK(ground.materials[0].texture(prefab::TextureSlot::kBaseColor) >= 0);
  CHECK(ground.materials[0].texture(prefab::TextureSlot::kOrm) >= 0);

  const prefab::Prefab target = Load("target.pbo");
  CHECK(target.bounds.size().y == doctest::Approx(1.0f));
  CHECK(target.materials[0].name == "steel");

  const prefab::Prefab crate = Load("crate.pbo");
  CHECK(crate.physics.mass == doctest::Approx(10.0f));
}

TEST_CASE(
    "The material library holds the preview, ground and steel "
    "materials") {
  const prefab::Prefab library = Load("materials.pbo");
  CHECK(library.meshes.empty());
  CHECK(library.materials.size() == 12);
  for (const char* name :
       {"preview dielectric r005", "preview metal r100", "ground", "steel"}) {
    CAPTURE(name);
    CHECK(prefab::FindMaterial(library, name) >= 0);
  }
}

TEST_CASE("range.mscn holds the default range and validates") {
  auto loaded = scene::LoadMscn(Asset("range.mscn"));
  REQUIRE(loaded.ok());
  const scene::SceneData& scene = loaded.value();
  CHECK(scene::ValidateSceneData(scene) == std::nullopt);
  CHECK(scene.instances.size() == 20);

  int targets = 0;
  for (const scene::Instance& instance : scene.instances) {
    if (instance.name.starts_with("target ")) {
      ++targets;
    }
  }
  CHECK(targets == 7);
  CHECK(scene.player_start.position.x == doctest::Approx(2.2f));
  CHECK(scene.player_start.position.y == doctest::Approx(1.6f));
  CHECK(scene.point_lights.size() == 1);
  CHECK(scene.environment.hdri_path.ends_with(".hdr"));

  // Every prefab exists and every override names real materials.
  std::vector<std::vector<std::string>> names;
  for (const std::string& path : scene.prefabs) {
    CAPTURE(path);
    auto prefab = prefab::LoadPrefab(path);
    REQUIRE(prefab.ok());
    std::vector<std::string>& materials = names.emplace_back();
    for (const prefab::Material& material : prefab.value().materials) {
      materials.push_back(material.name);
    }
  }
  CHECK(scene::ValidateAgainstPrefabs(scene, names).empty());
}

}  // namespace
}  // namespace blue
