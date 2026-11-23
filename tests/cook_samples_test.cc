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
#include <tuple>
#include <vector>

#include <glm/ext/quaternion_float.hpp>
#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "cook/prefab_split.h"
#include "core/path.h"
#include "physics/physics_runtime.h"
#include "physics/shape_cooking.h"
#include "platform/file_system.h"
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
      Near(sight.local.translation, glm::vec3(0.0f, 1.354f, -1.505f) * kScale));
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

// The transform of socket `name` at the rest pose, in prefab space.
glm::mat4 SocketWorld(const prefab::Prefab& prefab, std::string_view name) {
  const prefab::Socket& socket = Socket(prefab, name);
  glm::mat4 node(1.0f);
  if (socket.node >= 0) {
    node = cook::RestWorldMatrices(prefab)[static_cast<size_t>(socket.node)];
  }
  return node * animation::ToMatrix(socket.local);
}

// Vertex 0 of mesh `mesh` at the rest pose, in prefab space.
glm::vec3 MeshVertex(const prefab::Prefab& prefab, std::string_view mesh) {
  for (const prefab::Mesh& m : prefab.meshes) {
    if (m.name == mesh) {
      glm::mat4 node(1.0f);
      if (m.node >= 0) {
        node = cook::RestWorldMatrices(prefab)[static_cast<size_t>(m.node)];
      }
      return glm::vec3(node * glm::vec4(m.vertices[0].position, 1.0f));
    }
  }
  FAIL("no mesh ", mesh);
  return glm::vec3(0.0f);
}

TEST_CASE("The Glock parts are tagged and carry their sockets") {
  struct Expected {
    const char* file;
    const char* id;
    std::vector<const char*> sockets;
  };
  const Expected parts[] = {
      {"frame",
       "glock17_frame",
       {"grip", "slot.slide", "slot.barrel", "slot.magazine", "slot.tactical"}},
      {"slide",
       "glock17_slide",
       {"sight", "slot.sight_rear", "slot.sight_front"}},
      {"slide_mos",
       "glock17_slide_mos",
       {"sight", "slot.sight_rear", "slot.sight_front"}},
      {"barrel", "glock17_barrel", {"muzzle"}},
      {"barrel_threaded", "glock17_barrel_threaded", {"muzzle", "slot.muzzle"}},
      {"suppressor", "suppressor_9mm", {"mount", "muzzle"}},
      {"compensator", "compensator_9mm", {"mount", "muzzle"}},
      {"mag_17", "glock_mag_17", {}},
      {"mag_21", "glock_mag_21", {}},
      {"mag_33", "glock_mag_33", {}},
      {"sight_rear", "glock_sight_rear", {"sight"}},
      {"sight_front", "glock_sight_front", {}},
      {"reddot", "reddot_micro", {"sight", "lens"}},
  };
  for (const Expected& expected : parts) {
    const prefab::Prefab part =
        Load(std::string("weapons/glock17/parts/") + expected.file + ".pbo");
    CHECK(prefab::FindTag(part, "part") == expected.id);
    CHECK(!prefab::FindTag(part, "weapon"));
    CHECK(!part.meshes.empty());
    CHECK(part.sockets.size() == expected.sockets.size());
    for (const char* socket : expected.sockets) {
      CAPTURE(socket);
      CHECK(prefab::FindSocket(part, socket) >= 0);
    }
  }
  // The red dot's lens radius is the socket's scale.
  const prefab::Prefab dot = Load("weapons/glock17/parts/reddot.pbo");
  CHECK(Socket(dot, "lens").local.scale.x == doctest::Approx(0.0085f));
  // Longer magazines stick out further.
  const float mag17 = Load("weapons/glock17/parts/mag_17.pbo").bounds.min.y;
  const float mag33 = Load("weapons/glock17/parts/mag_33.pbo").bounds.min.y;
  CHECK(mag33 < mag17 - 0.08f);
}

TEST_CASE("The Glock frame part keeps the skeleton and the fire clip") {
  const prefab::Prefab frame = Load("weapons/glock17/parts/frame.pbo");
  const prefab::Prefab pistol = Load("glock17.pbo");
  CHECK(frame.hierarchy.nodes.size() == pistol.hierarchy.nodes.size());
  CHECK(frame.meshes.size() == 4);
  for (const char* mesh : {"Frame", "Trigger", "Frame_Parts", "Guide_Rod"}) {
    CAPTURE(mesh);
    bool found = false;
    for (const prefab::Mesh& m : frame.meshes) {
      found = found || m.name == mesh;
    }
    CHECK(found);
  }
  REQUIRE(frame.clips.size() == 1);
  CHECK(frame.clips[0].name == "fire");
  CHECK(animation::FindEvent(frame.clips[0], "shot") != nullptr);
  CHECK(animation::FindEvent(frame.clips[0], "slide_lock") != nullptr);
  const auto node_of = [&](std::string_view socket) {
    const int node = Socket(frame, socket).node;
    return node < 0 ? std::string()
                    : frame.hierarchy.nodes[static_cast<size_t>(node)].name;
  };
  CHECK(node_of("slot.slide") == "Slide");
  CHECK(node_of("slot.barrel") == "Barrel");
  CHECK(node_of("slot.magazine") == "Frame");

  // Mounted on their slot sockets, the slide and the barrel land where they
  // are in the whole pistol (turned so the bore points along +Z).
  const glm::quat turn = frame.hierarchy.nodes[0].rest.rotation;
  CHECK(Near(turn * glm::vec3(-1, 0, 0), {0, 0, 1}));
  for (const auto& [slot, file, mesh] :
       {std::tuple{"slot.slide", "slide", "Slide"},
        std::tuple{"slot.barrel", "barrel", "Barrel"}}) {
    CAPTURE(slot);
    const prefab::Prefab part =
        Load(std::string("weapons/glock17/parts/") + file + ".pbo");
    const glm::vec3 mounted = glm::vec3(
        SocketWorld(frame, slot) * glm::vec4(MeshVertex(part, mesh), 1.0f));
    CHECK(Near(mounted, turn * MeshVertex(pistol, mesh), 1.0e-5f));
    // The slot frame has the frame part's axes, scale 1.
    const glm::mat4 slot_world = SocketWorld(frame, slot);
    CHECK(Near(glm::vec3(slot_world[0]), {1, 0, 0}, 1.0e-5f));
    CHECK(Near(glm::vec3(slot_world[1]), {0, 1, 0}, 1.0e-5f));
  }
}

TEST_CASE("The parts catalogue is written") {
  auto text = platform::ReadFile(Asset("weapons/catalog/base.cfg"), 1 << 20);
  REQUIRE(text.ok());
  const std::string catalog(reinterpret_cast<const char*>(text.value().data()),
                            text.value().size());
  for (const char* section :
       {"[part glock17_frame]", "[part glock17_slide_mos]",
        "[part glock_mag_33]", "[part reddot_micro]", "[part suppressor_9mm]",
        "[part m16]", "[part spr_a3g]", "[preset glock17]", "[preset m16]",
        "[preset spr_a3g]", "legacy_type = m16", "slot.tactical = optional\n",
        "sighting_range = 25", "prefab = weapons/glock17/parts/frame.pbo"}) {
    CAPTURE(section);
    CHECK(catalog.find(section) != std::string::npos);
  }
}

}  // namespace
}  // namespace blue
