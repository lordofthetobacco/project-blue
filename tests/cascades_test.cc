#include "lighting/cascades.h"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace blue::lighting {
namespace {

constexpr float kNearPlane = 0.1f;
constexpr float kFov = 1.0471976f;  // 60 degrees.
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kTolerance = 1e-4f;

const glm::vec3 kSun = glm::normalize(glm::vec3(-0.4f, -1.0f, -0.3f));

glm::mat4 MakeView(const glm::vec3& position, float yaw, float pitch) {
  const glm::vec3 forward(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                          -std::cos(pitch) * std::cos(yaw));
  return glm::lookAt(position, position + forward, glm::vec3(0.0f, 1.0f, 0.0f));
}

// Returns the world-space corners of the camera frustum slice, computed
// independently of the code under test.
std::array<glm::vec3, 8> SliceCorners(const glm::mat4& view,
                                      float near_distance, float far_distance) {
  const glm::mat4 inverse_view = glm::inverse(view);
  const float tan_half = std::tan(kFov * 0.5f);
  std::array<glm::vec3, 8> corners;
  int index = 0;
  for (const float distance : {near_distance, far_distance}) {
    for (const float y : {-1.0f, 1.0f}) {
      for (const float x : {-1.0f, 1.0f}) {
        const glm::vec4 corner(x * distance * tan_half * kAspect,
                               y * distance * tan_half, -distance, 1.0f);
        corners[index++] = glm::vec3(inverse_view * corner);
      }
    }
  }
  return corners;
}

// Returns the ortho half extent r recovered from the cascade matrix: the first
// row of an orthonormal view times 1 / r has length 1 / r.
float HalfExtent(const Cascade& cascade) {
  const glm::mat4& m = cascade.view_projection;
  return 1.0f / glm::length(glm::vec3(m[0][0], m[1][0], m[2][0]));
}

bool IsFinite(const glm::mat4& m) {
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      if (!std::isfinite(m[column][row])) {
        return false;
      }
    }
  }
  return true;
}

// Checks that every corner of each cascade's slice lies in its clip volume.
void ExpectSlicesInsideCascades(const glm::mat4& view, const glm::vec3& sun) {
  const std::array<Cascade, kCascadeCount> cascades =
      FitCascades(view, kFov, kAspect, kNearPlane, sun);
  float split_near = kNearPlane;
  for (const Cascade& cascade : cascades) {
    REQUIRE(IsFinite(cascade.view_projection));
    for (const glm::vec3& corner :
         SliceCorners(view, split_near, cascade.split_far)) {
      const glm::vec4 clip = cascade.view_projection * glm::vec4(corner, 1.0f);
      CHECK(clip.w == doctest::Approx(1.0f));
      CHECK(clip.x >= -1.0f - kTolerance);
      CHECK(clip.x <= 1.0f + kTolerance);
      CHECK(clip.y >= -1.0f - kTolerance);
      CHECK(clip.y <= 1.0f + kTolerance);
      CHECK(clip.z >= -kTolerance);
      CHECK(clip.z <= 1.0f + kTolerance);
    }
    split_near = cascade.split_far;
  }
}

}  // namespace

TEST_CASE("cascade splits are increasing and end at the shadow distance") {
  const std::vector<float> splits = ComputeCascadeSplits(
      kNearPlane, kShadowDistance, kCascadeCount, kSplitLambda);
  REQUIRE(splits.size() == static_cast<size_t>(kCascadeCount));
  CHECK(splits.front() > kNearPlane);
  for (size_t i = 1; i < splits.size(); ++i) {
    CHECK(splits[i] > splits[i - 1]);
  }
  CHECK(splits.back() == kShadowDistance);
}

TEST_CASE("lambda 0 is uniform and lambda 1 is logarithmic") {
  constexpr int kCount = 4;
  const std::vector<float> uniform =
      ComputeCascadeSplits(0.5f, 100.0f, kCount, 0.0f);
  const std::vector<float> logarithmic =
      ComputeCascadeSplits(0.5f, 100.0f, kCount, 1.0f);
  for (int i = 0; i < kCount; ++i) {
    const float fraction = static_cast<float>(i + 1) / kCount;
    CHECK(uniform[i] == doctest::Approx(0.5f + 99.5f * fraction));
    CHECK(logarithmic[i] ==
          doctest::Approx(0.5f * std::pow(100.0f / 0.5f, fraction)));
  }
  CHECK(uniform.back() == 100.0f);
  CHECK(logarithmic.back() == 100.0f);
}

TEST_CASE("every slice corner projects inside its cascade") {
  SUBCASE("looking along the horizon") {
    ExpectSlicesInsideCascades(MakeView({0.0f, 1.7f, 0.0f}, 0.0f, 0.0f), kSun);
  }
  SUBCASE("rotated and pitched down") {
    ExpectSlicesInsideCascades(MakeView({12.0f, 30.0f, -7.0f}, 2.3f, -0.6f),
                               kSun);
  }
  SUBCASE("far from the origin looking up") {
    ExpectSlicesInsideCascades(MakeView({-400.0f, 5.0f, 650.0f}, -1.1f, 0.4f),
                               kSun);
  }
  SUBCASE("low sun against the view direction") {
    ExpectSlicesInsideCascades(MakeView({3.0f, 2.0f, 3.0f}, 0.7f, 0.1f),
                               glm::vec3(0.9f, -0.1f, 0.2f));
  }
}

TEST_CASE("texel_world_size is 2r / shadow map size") {
  const glm::mat4 view = MakeView({1.0f, 2.0f, 3.0f}, 0.4f, -0.2f);
  for (const Cascade& cascade :
       FitCascades(view, kFov, kAspect, kNearPlane, kSun)) {
    const float radius = HalfExtent(cascade);
    CHECK(cascade.texel_world_size ==
          doctest::Approx(2.0f * radius / kShadowMapSize).epsilon(1e-4));
  }
}

TEST_CASE("sub-texel camera motion moves the shadow map by whole texels") {
  const glm::vec3 start(20.0f, 1.7f, -15.0f);
  const std::array<Cascade, kCascadeCount> reference = FitCascades(
      MakeView(start, 0.5f, -0.1f), kFov, kAspect, kNearPlane, kSun);

  for (int step = 0; step < 12; ++step) {
    for (int i = 0; i < kCascadeCount; ++i) {
      const float texel = reference[i].texel_world_size;
      // A fraction of a texel per step along an arbitrary world direction.
      const glm::vec3 position = start + glm::vec3(0.31f, 0.0f, -0.17f) *
                                             texel * static_cast<float>(step);
      const std::array<Cascade, kCascadeCount> moved = FitCascades(
          MakeView(position, 0.5f, -0.1f), kFov, kAspect, kNearPlane, kSun);
      CHECK(moved[i].texel_world_size == reference[i].texel_world_size);

      const glm::vec4 origin = moved[i].view_projection * glm::vec4(0, 0, 0, 1);
      for (const float clip : {origin.x, origin.y}) {
        const float texels = clip * kShadowMapSize * 0.5f;
        CHECK(std::abs(texels - std::round(texels)) < 0.02f);
      }
    }
  }
}

TEST_CASE("rotating the camera in place keeps the cascade size") {
  const glm::vec3 position(5.0f, 2.0f, 5.0f);
  const std::array<Cascade, kCascadeCount> reference = FitCascades(
      MakeView(position, 0.0f, 0.0f), kFov, kAspect, kNearPlane, kSun);
  for (const float yaw : {0.3f, 1.7f, -2.4f, 3.0f}) {
    for (const float pitch : {-0.9f, 0.0f, 0.5f}) {
      const std::array<Cascade, kCascadeCount> rotated = FitCascades(
          MakeView(position, yaw, pitch), kFov, kAspect, kNearPlane, kSun);
      for (int i = 0; i < kCascadeCount; ++i) {
        CHECK(HalfExtent(rotated[i]) ==
              doctest::Approx(HalfExtent(reference[i])).epsilon(1e-4));
        CHECK(rotated[i].texel_world_size == reference[i].texel_world_size);
      }
    }
  }
}

TEST_CASE("a vertical sun produces valid cascades") {
  for (const glm::vec3 sun :
       {glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)}) {
    const glm::mat4 view = MakeView({0.0f, 1.7f, 0.0f}, 1.0f, -0.3f);
    for (const Cascade& cascade :
         FitCascades(view, kFov, kAspect, kNearPlane, sun)) {
      CHECK(IsFinite(cascade.view_projection));
      CHECK(cascade.texel_world_size > 0.0f);
    }
    ExpectSlicesInsideCascades(view, sun);
  }
}

}  // namespace blue::lighting
