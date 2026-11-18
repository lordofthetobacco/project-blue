#include "weapon/weapon_prefab.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <utility>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "animation/transform.h"
#include "core/path.h"
#include "core/result.h"
#include "prefab/prefab.h"
#include "prefab/prefab_file.h"
#include "weapon/weapon_animator.h"
#include "weapon/weapon_geometry.h"

namespace blue::weapon {
namespace {

constexpr float kTolerance = 1.0e-5f;

bool Near(glm::vec3 a, glm::vec3 b) { return glm::length(a - b) <= kTolerance; }

prefab::Socket MakeSocket(std::string name, glm::vec3 position, int node = -1) {
  prefab::Socket socket;
  socket.name = std::move(name);
  socket.node = node;
  socket.local.translation = position;
  return socket;
}

// A prefab with the three required sockets on the root.
prefab::Prefab MakeWeaponPrefab() {
  prefab::Prefab prefab;
  prefab.sockets.push_back(MakeSocket("muzzle", {0.0f, 0.05f, 0.7f}));
  prefab.sockets.push_back(MakeSocket("sight", {0.0f, 0.11f, -0.12f}));
  prefab.sockets.push_back(MakeSocket("grip", {0.0f, -0.06f, -0.15f}));
  return prefab;
}

TEST_CASE("GeometryFromSockets reads root sockets in metres") {
  prefab::Prefab prefab = MakeWeaponPrefab();
  const auto geometry = GeometryFromSockets(prefab);
  REQUIRE(geometry.ok());
  CHECK(geometry.value().scale == 1.0f);
  CHECK(Near(geometry.value().muzzle, {0.0f, 0.05f, 0.7f}));
  CHECK(Near(geometry.value().sight_eye_point, {0.0f, 0.11f, -0.12f}));
  CHECK(Near(geometry.value().sight_axis, {0.0f, 0.0f, 1.0f}));
  CHECK(Near(geometry.value().grip, {0.0f, -0.06f, -0.15f}));
  // No lens socket: iron sights.
  CHECK(geometry.value().optic_lens_radius == 0.0f);
  CHECK(Near(geometry.value().optic_lens, geometry.value().sight_eye_point));
}

TEST_CASE("GeometryFromSockets takes the sight axis from the rotation") {
  prefab::Prefab prefab = MakeWeaponPrefab();
  // +Z turned onto -X: a bore along -X like the Glock's.
  prefab.sockets[1].local.rotation =
      glm::angleAxis(glm::radians(-90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
  const auto geometry = GeometryFromSockets(prefab);
  REQUIRE(geometry.ok());
  CHECK(Near(geometry.value().sight_axis, {-1.0f, 0.0f, 0.0f}));
}

TEST_CASE("GeometryFromSockets reads the lens radius from its scale") {
  prefab::Prefab prefab = MakeWeaponPrefab();
  prefab::Socket lens = MakeSocket("lens", {0.0f, 0.104f, -0.098f});
  lens.local.scale = glm::vec3(0.0085f);
  prefab.sockets.push_back(lens);
  const auto geometry = GeometryFromSockets(prefab);
  REQUIRE(geometry.ok());
  CHECK(Near(geometry.value().optic_lens, {0.0f, 0.104f, -0.098f}));
  CHECK(geometry.value().optic_lens_radius ==
        doctest::Approx(0.0085f).epsilon(1.0e-4));
}

TEST_CASE("GeometryFromSockets follows a socket's node at rest") {
  prefab::Prefab prefab = MakeWeaponPrefab();
  animation::Node root;
  root.name = "root";
  root.rest.scale = glm::vec3(2.0f);
  animation::Node slide;
  slide.name = "slide";
  slide.parent = 0;
  slide.rest.translation = {1.0f, 0.0f, 0.0f};
  prefab.hierarchy.nodes = {root, slide};
  prefab.sockets[0].node = 1;
  prefab.sockets[0].local.translation = {0.0f, 0.5f, 0.0f};
  const auto geometry = GeometryFromSockets(prefab);
  REQUIRE(geometry.ok());
  // root scale 2 * (slide (1, 0, 0) + socket (0, 0.5, 0)).
  CHECK(Near(geometry.value().muzzle, {2.0f, 1.0f, 0.0f}));
}

TEST_CASE("GeometryFromSockets names a missing socket") {
  for (const char* name : {"muzzle", "sight", "grip"}) {
    CAPTURE(name);
    prefab::Prefab prefab = MakeWeaponPrefab();
    std::erase_if(prefab.sockets, [name](const prefab::Socket& socket) {
      return socket.name == name;
    });
    const auto geometry = GeometryFromSockets(prefab);
    REQUIRE_FALSE(geometry.ok());
    CHECK(geometry.error().find(std::string("'") + name + "'") !=
          std::string::npos);
  }
}

TEST_CASE("AnimatorTuningFromClip reads the shot and slide lock events") {
  animation::Clip clip;
  clip.name = "fire";
  clip.duration = 1.5f;
  animation::AddEvent(clip, {.name = "slide_lock", .time = 0.4f});
  animation::AddEvent(clip, {.name = "shot", .time = 0.333f});
  const auto tuning = AnimatorTuningFromClip(clip);
  REQUIRE(tuning.ok());
  CHECK(tuning.value().fire_time == doctest::Approx(0.333f));
  CHECK(tuning.value().lock_time == doctest::Approx(0.4f));
  const bool default_slide =
      tuning.value().slide_node == WeaponAnimatorTuning{}.slide_node;
  CHECK(default_slide);
}

TEST_CASE("AnimatorTuningFromClip names a missing event") {
  animation::Clip clip;
  clip.name = "fire";
  animation::AddEvent(clip, {.name = "shot", .time = 0.333f});
  const auto tuning = AnimatorTuningFromClip(clip);
  REQUIRE_FALSE(tuning.ok());
  CHECK(tuning.error().find("'slide_lock'") != std::string::npos);
}

// Returns the cooked sample weapon `name` (built by blue-pack samples).
prefab::Prefab LoadCookedWeapon(const char* name) {
  core::Result<prefab::Prefab, prefab::PrefabError> loaded =
      prefab::LoadPrefab(core::JoinPath(BLUE_ASSETS_DIR, name));
  REQUIRE(loaded.ok());
  return std::move(loaded).value();
}

// Checks that `cooked` (metres) is `authored` (model units) scaled.
void CheckScaled(const WeaponGeometry& cooked, const WeaponGeometry& authored) {
  constexpr float kMillimetre = 1.0e-3f;
  const float scale = authored.scale;
  CHECK(cooked.scale == 1.0f);
  CHECK(glm::length(cooked.muzzle - authored.muzzle * scale) < kMillimetre);
  CHECK(glm::length(cooked.sight_eye_point - authored.sight_eye_point * scale) <
        kMillimetre);
  CHECK(glm::length(cooked.grip - authored.grip * scale) < kMillimetre);
  CHECK(glm::dot(cooked.sight_axis, glm::normalize(authored.sight_axis)) >
        0.99999f);
  CHECK(cooked.optic_lens_radius ==
        doctest::Approx(authored.optic_lens_radius * scale).epsilon(1.0e-3));
  if (authored.optic_lens_radius > 0.0f) {
    CHECK(glm::length(cooked.optic_lens - authored.optic_lens * scale) <
          kMillimetre);
  }
}

TEST_CASE("The cooked M16 sockets match its calibration") {
  const auto geometry = GeometryFromSockets(LoadCookedWeapon("m16.pbo"));
  REQUIRE(geometry.ok());
  CheckScaled(geometry.value(), M16Geometry());
}

TEST_CASE("The cooked Glock sockets and fire events match its calibration") {
  const prefab::Prefab glock = LoadCookedWeapon("glock17.pbo");
  const auto geometry = GeometryFromSockets(glock);
  REQUIRE(geometry.ok());
  CheckScaled(geometry.value(), Glock17Geometry());
  const int fire = prefab::FindClip(glock, "fire");
  REQUIRE(fire >= 0);
  const auto tuning =
      AnimatorTuningFromClip(glock.clips[static_cast<size_t>(fire)]);
  REQUIRE(tuning.ok());
  CHECK(tuning.value().fire_time == doctest::Approx(0.333f));
  CHECK(tuning.value().lock_time == doctest::Approx(0.4f));
  CHECK(animation::FindNode(glock.hierarchy, "Slide") >= 0);
}

}  // namespace
}  // namespace blue::weapon
