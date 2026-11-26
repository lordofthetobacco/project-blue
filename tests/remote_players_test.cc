#include "game/remote_players.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "asset/mesh_data.h"
#include "physics/character.h"
#include "weapon/weapon_def.h"

namespace blue::game {
namespace {

constexpr float kEpsilon = 1.0e-4f;

glm::vec3 Apply(const glm::mat4& m, glm::vec3 point) {
  return glm::vec3(m * glm::vec4(point, 1.0f));
}

glm::vec3 Turn(const glm::mat4& m, glm::vec3 direction) {
  return glm::vec3(m * glm::vec4(direction, 0.0f));
}

TEST_CASE("MakeCapsule stands on the origin with outward faces") {
  const asset::MeshData capsule = asset::MakeCapsule(0.35f, 1.8f, 16);
  CHECK(capsule.bounds.min.y == doctest::Approx(0.0f));
  CHECK(capsule.bounds.max.y == doctest::Approx(1.8f));
  CHECK(capsule.bounds.max.x == doctest::Approx(0.35f));
  REQUIRE(capsule.indices.size() % 3 == 0);
  REQUIRE(capsule.submeshes.size() == 1);
  CHECK(capsule.submeshes[0].index_count == capsule.indices.size());
  float min_y = 10.0f;
  float max_y = -10.0f;
  for (const asset::Vertex& v : capsule.vertices) {
    CHECK(glm::length(v.normal) == doctest::Approx(1.0f));
    min_y = std::min(min_y, v.position.y);
    max_y = std::max(max_y, v.position.y);
    // Every vertex lies on the capsule's surface: radius from the axis
    // segment between the hemisphere centres.
    const float centre = std::clamp(v.position.y, 0.35f, 1.8f - 0.35f);
    const glm::vec3 axis(0.0f, centre, 0.0f);
    CHECK(glm::length(v.position - axis) ==
          doctest::Approx(0.35f).epsilon(0.001));
  }
  CHECK(min_y == doctest::Approx(0.0f).epsilon(0.001));
  CHECK(max_y == doctest::Approx(1.8f).epsilon(0.001));
  // Counter-clockwise from outside: each face normal points away from the
  // axis.
  for (size_t i = 0; i < capsule.indices.size(); i += 3) {
    const glm::vec3 a = capsule.vertices[capsule.indices[i]].position;
    const glm::vec3 b = capsule.vertices[capsule.indices[i + 1]].position;
    const glm::vec3 c = capsule.vertices[capsule.indices[i + 2]].position;
    const glm::vec3 normal = glm::cross(b - a, c - a);
    const glm::vec3 centroid = (a + b + c) / 3.0f;
    const glm::vec3 axis(0.0f, std::clamp(centroid.y, 0.35f, 1.45f), 0.0f);
    CHECK(glm::dot(normal, centroid - axis) > 0.0f);
  }
}

TEST_CASE("MakeCapsule clamps degenerate sizes") {
  const asset::MeshData capsule = asset::MakeCapsule(0.5f, 0.2f, 0);
  CHECK(capsule.bounds.max.y == doctest::Approx(1.0f));  // 2 * radius
  CHECK_FALSE(capsule.indices.empty());
}

TEST_CASE("remote players are placed upright, turned by their yaw") {
  const physics::CharacterDesc character;
  RemotePlayers players(character);
  const std::vector<RemotePlayerView> views = {
      {.id = 3,
       .feet = glm::vec3(1.0f, 0.0f, 2.0f),
       .yaw = std::numbers::pi_v<float> / 2.0f,
       .weapon = weapon::WeaponDefId{1}},
      {.id = 4, .feet = glm::vec3(5.0f, 0.0f, 0.0f), .alive = false},
      {.id = 5, .feet = glm::vec3(-2.0f, 0.0f, 0.0f), .crouching = true}};
  players.Update(views);
  REQUIRE(players.placements().size() == 2);  // The dead one is hidden.
  const RemotePlayerPlacement& standing = players.placements()[0];
  CHECK(standing.id == 3);
  CHECK(glm::length(Apply(standing.body, glm::vec3(0.0f)) -
                    glm::vec3(1.0f, 0.0f, 2.0f)) < kEpsilon);
  CHECK(Apply(standing.body, glm::vec3(0.0f, 1.0f, 0.0f)).y ==
        doctest::Approx(1.0f));
  CHECK(standing.name_anchor.y ==
        doctest::Approx(character.standing_height + kNameTagLift));
  // Yaw pi/2 turns -z (forward) to -x (yaw grows turning left).
  CHECK(glm::length(Turn(standing.body, glm::vec3(0.0f, 0.0f, -1.0f)) -
                    glm::vec3(-1.0f, 0.0f, 0.0f)) < kEpsilon);
  // The weapon prefab's +z points along the look; it sits in front of the
  // eye.
  REQUIRE(standing.weapon.has_value());
  CHECK(glm::length(Turn(standing.weapon_root, glm::vec3(0.0f, 0.0f, 1.0f)) -
                    glm::vec3(-1.0f, 0.0f, 0.0f)) < kEpsilon);
  const glm::vec3 hand = Apply(standing.weapon_root, glm::vec3(0.0f));
  CHECK(hand.x < 1.0f);  // Ahead of the player (towards -x).
  CHECK(hand.y > 1.0f);  // Around chest height.

  const RemotePlayerPlacement& crouched = players.placements()[1];
  CHECK(crouched.crouching);
  CHECK(crouched.name_anchor.y ==
        doctest::Approx(character.crouch_height + kNameTagLift));
  CHECK_FALSE(crouched.weapon.has_value());
  CHECK(players.height(true) < players.height(false));
}

TEST_CASE("remote player pitch tilts the weapon") {
  RemotePlayers players;
  const std::vector<RemotePlayerView> views = {
      {.id = 0, .pitch = std::numbers::pi_v<float> / 4.0f}};
  players.Update(views);
  REQUIRE(players.placements().size() == 1);
  const glm::vec3 forward =
      Turn(players.placements()[0].weapon_root, glm::vec3(0.0f, 0.0f, 1.0f));
  CHECK(forward.y == doctest::Approx(std::sqrt(0.5f)).epsilon(0.001));
  CHECK(forward.z == doctest::Approx(-std::sqrt(0.5f)).epsilon(0.001));
  players.Clear();
  CHECK(players.placements().empty());
}

}  // namespace
}  // namespace blue::game
