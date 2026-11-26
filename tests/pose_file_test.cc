#include "cook/pose_file.h"

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <string_view>

#include <glm/ext/quaternion_float.hpp>
#include <glm/vec3.hpp>

namespace blue::cook {
namespace {

constexpr std::string_view kValid =
    "# a comment\n"
    "weapon = m16.pbo\n"
    "weapon.translation = 0.1 1.2 -0.3\n"
    "weapon.rotation = 0 0 1 0\n"
    "\n"
    "hand_r.translation = 0.2 1 -0.4\r\n"
    "hand_r.rotation = 1 0 0 0\n"
    "hand_l.rotation = 0.7071068 0.7071068 0 0\n"
    "hand_l.translation = -0.2 1 -0.5\n";

std::string Replace(std::string_view text, std::string_view from,
                    std::string_view to) {
  std::string result(text);
  const size_t at = result.find(from);
  REQUIRE(at != std::string::npos);
  result.replace(at, from.size(), to);
  return result;
}

}  // namespace

TEST_CASE("A pose file gives the weapon transform and the sockets") {
  auto parsed = ParsePoseFile(kValid);
  REQUIRE(parsed.ok());
  const PoseFile& pose = parsed.value();
  CHECK(pose.weapon == "m16.pbo");
  CHECK(pose.weapon_local.translation == glm::vec3(0.1f, 1.2f, -0.3f));
  CHECK(pose.weapon_local.rotation == glm::quat(0.0f, 0.0f, 1.0f, 0.0f));
  CHECK(pose.weapon_local.scale == glm::vec3(1.0f));
  REQUIRE(pose.sockets.size() == 2);
  CHECK(pose.sockets[0].name == "hand_r");
  CHECK(pose.sockets[0].node == -1);
  CHECK(pose.sockets[0].local.translation == glm::vec3(0.2f, 1.0f, -0.4f));
  CHECK(pose.sockets[1].name == "hand_l");
  CHECK(pose.sockets[1].local.translation == glm::vec3(-0.2f, 1.0f, -0.5f));
  // Normalised.
  const glm::quat q = pose.sockets[1].local.rotation;
  CHECK(std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z) ==
        doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("A broken pose file is refused with the line") {
  struct Broken {
    std::string text;
    std::string_view message;
  };
  const Broken cases[] = {
      {Replace(kValid, "weapon = m16.pbo\n", ""), "no weapon"},
      {Replace(kValid, "weapon.translation = 0.1 1.2 -0.3\n", ""),
       "'weapon' needs both"},
      {Replace(kValid, "weapon.translation = 0.1 1.2 -0.3\n",
               "weapon.translation = 0.1 1.2\n"),
       "line 3: bad or repeated translation"},
      {Replace(kValid, "weapon.translation = 0.1 1.2 -0.3\n",
               "weapon.translation = 0.1 1.2 -0.3 4\n"),
       "line 3: bad or repeated translation"},
      {Replace(kValid, "0 0 1 0", "0 0 2 0"), "not a unit quaternion"},
      {Replace(kValid, "0 0 1 0", "0 0 nan 0"), "bad or repeated rotation"},
      {Replace(kValid, "hand_r.rotation = 1 0 0 0\n", ""),
       "'hand_r' needs both"},
      {Replace(kValid, "hand_l.translation", "hand_r.translation"),
       "line 9: bad or repeated translation"},
      {Replace(kValid, "# a comment", "colour = red"),
       "line 1: unknown key 'colour'"},
      {Replace(kValid, "# a comment", "no equals sign"), "line 1: no '='"},
      {Replace(kValid, "# a comment", "weapon = glock17.pbo"),
       "weapon given twice"},
      {Replace(kValid,
               "weapon.translation = 0.1 1.2 -0.3\n"
               "weapon.rotation = 0 0 1 0\n",
               ""),
       "no weapon transform"},
  };
  for (const Broken& broken : cases) {
    CAPTURE(broken.text);
    auto parsed = ParsePoseFile(broken.text);
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().find(broken.message) != std::string::npos);
  }
}

TEST_CASE("A missing pose file is an error naming it") {
  auto loaded = LoadPoseFile("no/such/figure.pose");
  REQUIRE_FALSE(loaded.ok());
  CHECK(loaded.error().find("figure.pose") != std::string::npos);
}

}  // namespace blue::cook
