#include "asset/animated_model.h"

#include <doctest/doctest.h>

#include <array>
#include <string>
#include <vector>

#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "animation/clip.h"
#include "animation/hierarchy.h"
#include "core/path.h"
#include "platform/file_system.h"

namespace blue::asset {
namespace {

constexpr float kEpsilon = 1.0e-3f;

bool NearlyEqual(const glm::vec3& a, const glm::vec3& b) {
  return glm::all(glm::lessThanEqual(glm::abs(a - b), glm::vec3(kEpsilon)));
}

std::string GlockPath() {
  return core::JoinPath(BLUE_SAMPLES_DIR, "glock/glock17.glb");
}

// Returns the world matrices of the rest pose of `model`.
std::vector<glm::mat4> RestWorld(const AnimatedModel& model) {
  std::vector<animation::Transform> pose(model.hierarchy.nodes.size());
  animation::ResetToRest(model.hierarchy, pose);
  std::vector<glm::mat4> world(pose.size());
  animation::ComputeWorldMatrices(model.hierarchy, pose, world);
  return world;
}

}  // namespace

TEST_CASE("PoseBounds transforms each part's bounds by its node") {
  std::array<ModelPart, 2> parts;
  parts[0].node = 0;
  parts[0].mesh.bounds = {.min = {-1, -1, -1}, .max = {1, 1, 1}};
  parts[1].node = 1;
  parts[1].mesh.bounds = {.min = {0, 0, 0}, .max = {1, 2, 3}};
  const std::array<glm::mat4, 2> world{
      glm::mat4(1.0f),
      glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 0.0f, 0.0f))};

  const Bounds bounds = PoseBounds(parts, world);

  CHECK(bounds.min == glm::vec3(-1, -1, -1));
  CHECK(bounds.max == glm::vec3(11, 2, 3));
}

TEST_CASE("ImportAnimatedModel reports a missing file") {
  const auto result = ImportAnimatedModel(
      core::JoinPath(platform::TempDirectory(), "blue_no_such_model.glb"));
  REQUIRE_FALSE(result.ok());
  CHECK((result.error() == ImportError::kFileNotFound));
}

TEST_CASE("ImportAnimatedModel loads a static model without clips") {
  const auto result =
      ImportAnimatedModel(core::JoinPath(BLUE_SAMPLES_DIR, "teapot.obj"));
  REQUIRE(result.ok());
  const AnimatedModel& model = result.value();
  CHECK(model.clips.empty());
  REQUIRE_FALSE(model.parts.empty());
  CHECK(animation::IsValid(model.hierarchy));
  size_t indices = 0;
  for (const ModelPart& part : model.parts) {
    indices += part.mesh.indices.size();
  }
  CHECK(indices == 18960);
  CHECK(NearlyEqual(model.rest_bounds.min, {-3.0f, 0.0f, -2.0f}));
  CHECK(NearlyEqual(model.rest_bounds.max, {3.434f, 3.15f, 2.0f}));
}

TEST_CASE("ImportAnimatedModel loads the Glock 17 with its animations") {
  const auto result = ImportAnimatedModel(GlockPath());
  REQUIRE(result.ok());
  const AnimatedModel& model = result.value();
  const animation::Hierarchy& hierarchy = model.hierarchy;
  REQUIRE(animation::IsValid(hierarchy));

  SUBCASE("hierarchy and parts") {
    for (const char* name :
         {"Pistol_Root", "Frame", "Slide", "Barrel", "Trigger", "Guide_Rod",
          "Frame_Parts", "Glock_parts_ORIGINAL_combined"}) {
      CAPTURE(name);
      CHECK(animation::FindNode(hierarchy, name) >= 0);
    }
    const int frame = animation::FindNode(hierarchy, "Frame");
    CHECK(
        hierarchy
            .nodes[static_cast<size_t>(animation::FindNode(hierarchy, "Slide"))]
            .parent == frame);
    CHECK(model.parts.size() == 7);
    for (const ModelPart& part : model.parts) {
      CHECK(part.node >= 0);
      CHECK(static_cast<size_t>(part.node) < hierarchy.nodes.size());
      CHECK_FALSE(part.mesh.vertices.empty());
      CHECK(part.mesh.indices.size() % 3 == 0);
    }
  }

  SUBCASE("rest bounds match the posed vertices") {
    const std::vector<glm::mat4> world = RestWorld(model);
    Bounds bounds{.min = glm::vec3(1.0e9f), .max = glm::vec3(-1.0e9f)};
    for (const ModelPart& part : model.parts) {
      const glm::mat4& matrix = world[static_cast<size_t>(part.node)];
      for (const Vertex& vertex : part.mesh.vertices) {
        const glm::vec3 p = glm::vec3(matrix * glm::vec4(vertex.position, 1));
        bounds.min = glm::min(bounds.min, p);
        bounds.max = glm::max(bounds.max, p);
      }
    }
    CHECK(NearlyEqual(bounds.min, model.rest_bounds.min));
    CHECK(NearlyEqual(bounds.max, model.rest_bounds.max));
    // No rotation at rest, so the box-corner bound is exact too.
    const Bounds posed = PoseBounds(model.parts, world);
    CHECK(NearlyEqual(posed.min, model.rest_bounds.min));
    CHECK(NearlyEqual(posed.max, model.rest_bounds.max));
    MESSAGE("rest bounds min (", model.rest_bounds.min.x, ", ",
            model.rest_bounds.min.y, ", ", model.rest_bounds.min.z, ") max (",
            model.rest_bounds.max.x, ", ", model.rest_bounds.max.y, ", ",
            model.rest_bounds.max.z, ")");
  }

  SUBCASE("vertex colours come from the base-colour textures") {
    bool all_in_range = true;
    bool any_not_white = false;
    glm::vec3 lowest(1.0f);
    glm::vec3 highest(0.0f);
    glm::vec3 sum(0.0f);
    size_t count = 0;
    for (const ModelPart& part : model.parts) {
      for (const Vertex& vertex : part.mesh.vertices) {
        all_in_range =
            all_in_range &&
            glm::all(glm::greaterThanEqual(vertex.color, glm::vec3(0.0f))) &&
            glm::all(glm::lessThanEqual(vertex.color, glm::vec3(1.0f)));
        any_not_white = any_not_white || vertex.color != glm::vec3(1.0f);
        lowest = glm::min(lowest, vertex.color);
        highest = glm::max(highest, vertex.color);
        sum += vertex.color;
        ++count;
      }
    }
    CHECK(all_in_range);
    CHECK(any_not_white);
    // Texture detail, not one flat factor colour.
    CHECK(highest.r - lowest.r > 0.05f);
    const glm::vec3 mean = sum / static_cast<float>(count);
    MESSAGE("mean vertex colour (", mean.x, ", ", mean.y, ", ", mean.z, ")");
  }

  SUBCASE("the per-node actions merge into one firing clip") {
    REQUIRE(model.clips.size() == 4);
    for (const char* name : {"Pistol_RootAction", "SlideAction", "BarrelAction",
                             "TriggerAction"}) {
      CAPTURE(name);
      CHECK(FindClip(model, name) >= 0);
    }
    const animation::Clip fire = animation::MergeClips(model.clips, "fire");
    CHECK(fire.duration == doctest::Approx(1.5f));
    for (const animation::Channel& channel : fire.channels) {
      CHECK(animation::IsValid(channel));
    }

    const int slide = animation::FindNode(hierarchy, "Slide");
    std::vector<animation::Transform> pose(hierarchy.nodes.size());
    animation::ResetToRest(hierarchy, pose);
    animation::SampleClip(fire, 0.0f, pose);
    CHECK(pose[static_cast<size_t>(slide)].translation.x ==
          doctest::Approx(-0.370f).epsilon(0.002));
    animation::SampleClip(fire, 0.4f, pose);
    CHECK(pose[static_cast<size_t>(slide)].translation.x ==
          doctest::Approx(0.130f).epsilon(0.01));
    animation::SampleClip(fire, 1.5f, pose);
    CHECK(pose[static_cast<size_t>(slide)].translation.x ==
          doctest::Approx(-0.370f).epsilon(0.002));
  }
}

}  // namespace blue::asset
