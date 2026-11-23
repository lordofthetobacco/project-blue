#include "render/temporal_math.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include <glm/ext/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/trigonometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "camera/projection.h"
#include "core/extent.h"
#include "render/scene_view.h"

namespace blue::render {
namespace {

constexpr float kNan = std::numeric_limits<float>::quiet_NaN();
// What an Apple GPU's temporal scaler reports.
constexpr float kMinScale = 1.0f;
constexpr float kMaxScale = 3.0f;
constexpr core::Extent2D kOutput{2560, 1440};

constexpr Upscaling kAllModes[] = {Upscaling::kOff, Upscaling::kNative,
                                   Upscaling::kQuality, Upscaling::kBalanced,
                                   Upscaling::kPerformance};

// Returns where `clip` lands on a target of `extent` pixels (x right, y
// down).
glm::vec2 PixelOf(const glm::vec4& clip, core::Extent2D extent) {
  const glm::vec2 ndc = glm::vec2(clip) / clip.w;
  return {(ndc.x * 0.5f + 0.5f) * static_cast<float>(extent.width),
          (0.5f - ndc.y * 0.5f) * static_cast<float>(extent.height)};
}

glm::mat4 TestProjection(core::Extent2D extent) {
  return camera::ReversedInfinitePerspective(
      glm::radians(60.0f),
      static_cast<float>(extent.width) / static_cast<float>(extent.height),
      0.05f);
}

TEST_CASE("the mode list covers every Upscaling value") {
  CHECK(std::size(kAllModes) == static_cast<size_t>(kUpscalingModeCount));
  for (int32_t i = 0; i < kUpscalingModeCount; ++i) {
    CHECK(static_cast<int32_t>(kAllModes[i]) == i);
  }
}

TEST_CASE("only kOff runs without the temporal scaler") {
  CHECK_FALSE(UsesTemporalScaler(Upscaling::kOff));
  CHECK(UsesTemporalScaler(Upscaling::kNative));
  CHECK(UsesTemporalScaler(Upscaling::kQuality));
  CHECK(UsesTemporalScaler(Upscaling::kBalanced));
  CHECK(UsesTemporalScaler(Upscaling::kPerformance));
}

TEST_CASE("FXAA is applied only when enabled and the scaler is off") {
  PostSettings post;
  CHECK(post.upscaling == Upscaling::kOff);  // The default keeps FXAA.
  CHECK(FxaaActive(post));
  post.fxaa_enabled = false;
  CHECK_FALSE(FxaaActive(post));
  post.fxaa_enabled = true;
  for (const Upscaling mode : kAllModes) {
    post.upscaling = mode;
    CHECK(FxaaActive(post) == (mode == Upscaling::kOff));
  }
}

TEST_CASE("every mode has its own UI name") {
  std::vector<std::string_view> names;
  for (const Upscaling mode : kAllModes) {
    const std::string_view name = UpscalingName(mode);
    CHECK_FALSE(name.empty());
    CHECK(std::find(names.begin(), names.end(), name) == names.end());
    names.push_back(name);
  }
  CHECK(std::string_view(UpscalingName(Upscaling::kOff)) == "Off");
  // An out-of-range value (a corrupt setting) reads as off.
  CHECK(std::string_view(UpscalingName(static_cast<Upscaling>(99))) == "Off");
  CHECK(UpscalingRatio(static_cast<Upscaling>(99)) == 1.0f);
}

TEST_CASE("the nominal ratios") {
  CHECK(UpscalingRatio(Upscaling::kOff) == 1.0f);
  CHECK(UpscalingRatio(Upscaling::kNative) == 1.0f);
  CHECK(UpscalingRatio(Upscaling::kQuality) == 1.5f);
  CHECK(UpscalingRatio(Upscaling::kBalanced) == 1.7f);
  CHECK(UpscalingRatio(Upscaling::kPerformance) == 2.0f);
}

TEST_CASE("off and native render at the output resolution") {
  CHECK(RenderExtent(kOutput, Upscaling::kOff, kMinScale, kMaxScale) ==
        kOutput);
  CHECK(RenderExtent(kOutput, Upscaling::kNative, kMinScale, kMaxScale) ==
        kOutput);
  // Whatever the scaler supports.
  CHECK(RenderExtent(kOutput, Upscaling::kNative, 1.5f, 2.0f) == kOutput);
  CHECK(RenderExtent(kOutput, Upscaling::kOff, kNan, kNan) == kOutput);
}

TEST_CASE("the upscaling modes divide each side by their ratio") {
  CHECK(RenderExtent(kOutput, Upscaling::kQuality, kMinScale, kMaxScale) ==
        core::Extent2D{1707, 960});
  CHECK(RenderExtent(kOutput, Upscaling::kBalanced, kMinScale, kMaxScale) ==
        core::Extent2D{1506, 847});
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, kMinScale, kMaxScale) ==
        core::Extent2D{1280, 720});
}

TEST_CASE("the ratio is clamped to what the scaler supports") {
  // At most 1.5x: balanced and performance render like quality.
  const core::Extent2D quality =
      RenderExtent(kOutput, Upscaling::kQuality, 1.0f, 1.5f);
  CHECK(RenderExtent(kOutput, Upscaling::kBalanced, 1.0f, 1.5f) == quality);
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, 1.0f, 1.5f) == quality);
  // At least 1.8x: quality renders smaller than it asks for.
  CHECK(RenderExtent(kOutput, Upscaling::kQuality, 1.8f, 3.0f) ==
        core::Extent2D{1422, 800});
  // No scaling supported at all.
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, 1.0f, 1.0f) == kOutput);
}

TEST_CASE("rounding never takes a side's ratio out of the supported range") {
  for (uint32_t side = 1; side <= 2000; ++side) {
    const core::Extent2D output{side, 2001 - side};
    for (const Upscaling mode : kAllModes) {
      for (const float max_scale : {1.0f, 1.5f, 1.7f, 2.0f, 3.0f}) {
        const core::Extent2D render =
            RenderExtent(output, mode, kMinScale, max_scale);
        CAPTURE(side);
        CAPTURE(static_cast<int>(mode));
        CAPTURE(max_scale);
        REQUIRE(render.width >= 1);
        REQUIRE(render.height >= 1);
        REQUIRE(render.width <= output.width);
        REQUIRE(render.height <= output.height);
        REQUIRE(static_cast<float>(output.width) /
                    static_cast<float>(render.width) <=
                max_scale);
        REQUIRE(static_cast<float>(output.height) /
                    static_cast<float>(render.height) <=
                max_scale);
      }
    }
  }
}

TEST_CASE("degenerate outputs and ranges give a usable render extent") {
  CHECK(RenderExtent({0, 0}, Upscaling::kPerformance, kMinScale, kMaxScale) ==
        core::Extent2D{0, 0});
  CHECK(RenderExtent({1, 1}, Upscaling::kPerformance, kMinScale, kMaxScale) ==
        core::Extent2D{1, 1});
  // Bad ranges: NaN, non-positive, inverted.
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, kNan, kNan) == kOutput);
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, 0.0f, 0.0f) == kOutput);
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, 1.0f, 0.5f) == kOutput);
  CHECK(RenderExtent(kOutput, Upscaling::kPerformance, -1.0f, 3.0f) ==
        core::Extent2D{1280, 720});
}

TEST_CASE("the jitter sequence grows with the square of the ratio") {
  const auto count = [](Upscaling mode) {
    return JitterPhaseCount(
        RenderExtent(kOutput, mode, kMinScale, kMaxScale), kOutput);
  };
  CHECK(count(Upscaling::kNative) == 8);
  CHECK(count(Upscaling::kQuality) == 18);
  CHECK(count(Upscaling::kBalanced) == 24);
  CHECK(count(Upscaling::kPerformance) == 32);
  CHECK(JitterPhaseCount({0, 0}, kOutput) == kJitterBasePhaseCount);
  CHECK(JitterPhaseCount(kOutput, {0, 0}) == kJitterBasePhaseCount);
  // Rendering above the output resolution is not a case; no shorter than
  // the base.
  CHECK(JitterPhaseCount(kOutput, {1280, 720}) == kJitterBasePhaseCount);
}

TEST_CASE("jitter offsets are the Halton (2, 3) points around the centre") {
  CHECK(JitterOffset(0, 8).x == doctest::Approx(0.0f));
  CHECK(JitterOffset(0, 8).y == doctest::Approx(1.0f / 3.0f - 0.5f));
  CHECK(JitterOffset(1, 8).x == doctest::Approx(-0.25f));
  CHECK(JitterOffset(1, 8).y == doctest::Approx(2.0f / 3.0f - 0.5f));
  CHECK(JitterOffset(2, 8).x == doctest::Approx(0.25f));
  CHECK(JitterOffset(2, 8).y == doctest::Approx(1.0f / 9.0f - 0.5f));
}

TEST_CASE("the jitter sequence stays inside the pixel, repeats, is even") {
  for (const uint32_t count : {8u, 18u, 24u, 32u}) {
    glm::vec2 sum(0.0f);
    std::vector<glm::vec2> seen;
    for (uint32_t frame = 0; frame < count; ++frame) {
      const glm::vec2 jitter = JitterOffset(frame, count);
      CHECK(std::abs(jitter.x) < 0.5f);
      CHECK(std::abs(jitter.y) < 0.5f);
      // Every phase samples a different place.
      for (const glm::vec2 other : seen) {
        CHECK((other.x != jitter.x || other.y != jitter.y));
      }
      seen.push_back(jitter);
      sum += jitter;
      // The sequence repeats every `count` frames.
      CHECK(JitterOffset(frame + count, count) == jitter);
      CHECK(JitterOffset(frame + 1000ull * count, count) == jitter);
    }
    // Centred on the pixel centre: no net shift of the image.
    CAPTURE(count);
    CHECK(std::abs(sum.x / static_cast<float>(count)) < 0.08f);
    CHECK(std::abs(sum.y / static_cast<float>(count)) < 0.08f);
    // Both halves of the pixel are sampled equally often along x (base 2)
    // when the count is even.
    const auto left = std::count_if(seen.begin(), seen.end(),
                                    [](glm::vec2 j) { return j.x < 0.0f; });
    CHECK(left * 2 >= static_cast<long>(count) - 2);
    CHECK(left * 2 <= static_cast<long>(count) + 2);
  }
}

TEST_CASE("a phase count of 0 is a sequence of one") {
  CHECK(JitterOffset(0, 0) == JitterOffset(0, 1));
  CHECK(JitterOffset(5, 0) == JitterOffset(0, 1));
}

TEST_CASE("the jittered projection moves every point by the jitter") {
  const core::Extent2D extent{1280, 720};
  const glm::mat4 projection = TestProjection(extent);
  const glm::vec2 jitter(0.3f, -0.2f);
  const glm::mat4 jittered = JitterProjection(projection, jitter, extent);
  // View-space points near and far, on and off the axis.
  for (const glm::vec3 point :
       {glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.4f, -0.3f, -2.0f),
        glm::vec3(-30.0f, 12.0f, -80.0f), glm::vec3(0.01f, 0.02f, -0.06f)}) {
    const glm::vec4 clip = projection * glm::vec4(point, 1.0f);
    const glm::vec4 moved = jittered * glm::vec4(point, 1.0f);
    const glm::vec2 shift = PixelOf(moved, extent) - PixelOf(clip, extent);
    CHECK(shift.x == doctest::Approx(jitter.x).epsilon(0.01));
    CHECK(shift.y == doctest::Approx(jitter.y).epsilon(0.01));
    // Depth and w are untouched: depth tests and reversed Z are unaffected.
    CHECK(moved.z == clip.z);
    CHECK(moved.w == clip.w);
  }
}

TEST_CASE("positive jitter is right and down on the target") {
  const core::Extent2D extent{100, 50};
  const glm::mat4 projection = TestProjection(extent);
  const glm::vec4 centre(0.0f, 0.0f, -1.0f, 1.0f);
  const glm::vec2 before = PixelOf(projection * centre, extent);
  CHECK(before.x == doctest::Approx(50.0f));
  CHECK(before.y == doctest::Approx(25.0f));
  const glm::vec2 right = PixelOf(
      JitterProjection(projection, {0.5f, 0.0f}, extent) * centre, extent);
  CHECK(right.x == doctest::Approx(50.5f));
  CHECK(right.y == doctest::Approx(25.0f));
  const glm::vec2 down = PixelOf(
      JitterProjection(projection, {0.0f, 0.5f}, extent) * centre, extent);
  CHECK(down.x == doctest::Approx(50.0f));
  CHECK(down.y == doctest::Approx(25.5f));
}

TEST_CASE("no jitter leaves the projection bit for bit") {
  const core::Extent2D extent{1280, 720};
  const glm::mat4 projection = TestProjection(extent);
  CHECK(JitterProjection(projection, glm::vec2(0.0f), extent) == projection);
}

TEST_CASE("jitter also shifts an orthographic projection") {
  const core::Extent2D extent{200, 100};
  // x and y in [-1, 1] map straight to NDC; w stays 1.
  const glm::mat4 projection(1.0f);
  const glm::mat4 jittered =
      JitterProjection(projection, {0.25f, 0.25f}, extent);
  const glm::vec4 point(0.5f, 0.5f, 0.5f, 1.0f);
  const glm::vec2 shift =
      PixelOf(jittered * point, extent) - PixelOf(projection * point, extent);
  CHECK(shift.x == doctest::Approx(0.25f));
  CHECK(shift.y == doctest::Approx(0.25f));
}

TEST_CASE("the texture LOD bias is log2 of the resolution ratio") {
  CHECK(TextureLodBias(kOutput, kOutput) == 0.0f);
  CHECK(TextureLodBias({1280, 720}, kOutput) == doctest::Approx(-1.0f));
  CHECK(TextureLodBias(
            RenderExtent(kOutput, Upscaling::kQuality, kMinScale, kMaxScale),
            kOutput) == doctest::Approx(-0.585f).epsilon(0.01));
  CHECK(TextureLodBias(
            RenderExtent(kOutput, Upscaling::kBalanced, kMinScale, kMaxScale),
            kOutput) == doctest::Approx(-0.766f).epsilon(0.01));
}

TEST_CASE("the texture LOD bias is never positive and is limited") {
  // Rendering above the output resolution blurs nothing.
  CHECK(TextureLodBias(kOutput, {1280, 720}) == 0.0f);
  CHECK(TextureLodBias({16, 9}, kOutput) == kMinTextureLodBias);
  CHECK(TextureLodBias({0, 0}, kOutput) == 0.0f);
  CHECK(TextureLodBias(kOutput, {0, 0}) == 0.0f);
}

TEST_CASE("LOD biases snap to the nearest quarter level") {
  CHECK(TextureLodBiasStep(0.0f) == 0);
  CHECK(TextureLodBiasStep(-0.1f) == 0);
  CHECK(TextureLodBiasStep(-0.13f) == 1);
  CHECK(TextureLodBiasStep(-0.585f) == 2);  // Quality: -0.5
  CHECK(TextureLodBiasStep(-0.766f) == 3);  // Balanced: -0.75
  CHECK(TextureLodBiasStep(-1.0f) == 4);    // Performance: -1
  CHECK(TextureLodBiasStep(-2.0f) == kTextureLodBiasStepCount - 1);
  // Out of range.
  CHECK(TextureLodBiasStep(-7.0f) == kTextureLodBiasStepCount - 1);
  CHECK(TextureLodBiasStep(0.5f) == 0);
  CHECK(TextureLodBiasStep(kNan) == 0);
}

TEST_CASE("every LOD bias step is its own bias") {
  for (size_t step = 0; step < kTextureLodBiasStepCount; ++step) {
    const float bias = TextureLodBiasOfStep(step);
    CHECK(bias <= 0.0f);
    CHECK(bias >= kMinTextureLodBias);
    CHECK(TextureLodBiasStep(bias) == step);
  }
  CHECK(TextureLodBiasOfStep(0) == 0.0f);
  CHECK(TextureLodBiasOfStep(4) == -1.0f);
}

TEST_CASE("a point that does not move has no motion vector") {
  const glm::vec4 clip(0.3f, -0.2f, 0.1f, 2.0f);
  CHECK(MotionVector(clip, clip) == glm::vec2(0.0f));
  // The same NDC position at another depth does not move on screen either.
  const glm::vec2 same = MotionVector(clip, clip * 3.0f);
  CHECK(same.x == doctest::Approx(0.0f));
  CHECK(same.y == doctest::Approx(0.0f));
}

TEST_CASE("the motion vector points from now to the previous position") {
  const core::Extent2D extent{1280, 720};
  const glm::vec2 size(extent.width, extent.height);
  const glm::mat4 projection = TestProjection(extent);
  const glm::vec4 previous_point(0.1f, 0.2f, -4.0f, 1.0f);
  // Ten pixels to the right and down on screen from there.
  const glm::vec4 previous_clip = projection * previous_point;
  const glm::vec2 previous_pixel = PixelOf(previous_clip, extent);
  glm::vec4 clip = previous_clip;
  clip.x += 2.0f * 10.0f / size.x * clip.w;
  clip.y -= 2.0f * 10.0f / size.y * clip.w;
  const glm::vec2 moved = PixelOf(clip, extent) - previous_pixel;
  REQUIRE(moved.x == doctest::Approx(10.0f));
  REQUIRE(moved.y == doctest::Approx(10.0f));

  // In pixels (scaled by the render extent, as the scaler does): the
  // header's example, (-10, -10).
  const glm::vec2 motion = MotionVector(clip, previous_clip) * size;
  CHECK(motion.x == doctest::Approx(-10.0f).epsilon(0.001));
  CHECK(motion.y == doctest::Approx(-10.0f).epsilon(0.001));
  // The pixel plus its vector is where the point was.
  const glm::vec2 back = PixelOf(clip, extent) + motion;
  CHECK(back.x == doctest::Approx(previous_pixel.x).epsilon(0.0001));
  CHECK(back.y == doctest::Approx(previous_pixel.y).epsilon(0.0001));
}

TEST_CASE("a camera turn gives static geometry the opposite motion") {
  const core::Extent2D extent{1000, 1000};
  const glm::mat4 projection = TestProjection(extent);
  const glm::vec4 world(0.0f, 0.0f, -10.0f, 1.0f);
  // The camera yaws to its left (positive rotation about +Y): the view
  // matrix turns the world to the right.
  const glm::mat4 view = glm::rotate(glm::mat4(1.0f), glm::radians(-1.0f),
                                     glm::vec3(0.0f, 1.0f, 0.0f));
  const glm::vec4 previous_clip = projection * world;
  const glm::vec4 clip = projection * view * world;
  REQUIRE(PixelOf(clip, extent).x > PixelOf(previous_clip, extent).x);
  const glm::vec2 motion = MotionVector(clip, previous_clip);
  CHECK(motion.x < 0.0f);  // It came from the left.
  CHECK(motion.y == doctest::Approx(0.0f));
}

TEST_CASE("a motion vector is always finite") {
  const glm::vec4 front(0.1f, 0.1f, 0.1f, 1.0f);
  const float inf = std::numeric_limits<float>::infinity();
  for (const glm::vec4 bad :
       {glm::vec4(inf, 0.0f, 0.1f, 1.0f), glm::vec4(0.0f, kNan, 0.1f, 1.0f),
        glm::vec4(3.0e38f, 3.0e38f, 0.1f, 1.0e-5f)}) {
    CHECK(MotionVector(front, bad) == glm::vec2(0.0f));
    CHECK(MotionVector(bad, front) == glm::vec2(0.0f));
  }
}

TEST_CASE("points behind the camera have no motion vector") {
  const glm::vec4 front(0.1f, 0.1f, 0.1f, 1.0f);
  const glm::vec4 other(0.5f, 0.4f, 0.1f, 1.0f);
  REQUIRE(MotionVector(front, other) != glm::vec2(0.0f));
  for (const float w : {0.0f, -1.0f, kMotionVectorMinClipW, kNan}) {
    glm::vec4 behind = other;
    behind.w = w;
    CHECK(MotionVector(front, behind) == glm::vec2(0.0f));
    CHECK(MotionVector(behind, front) == glm::vec2(0.0f));
  }
}

}  // namespace
}  // namespace blue::render
