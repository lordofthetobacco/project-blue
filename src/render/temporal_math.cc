#include "render/temporal_math.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/extent.h"
#include "render/scene_view.h"

namespace blue::render {

namespace {

// Returns element `index` (from 1) of the Halton sequence of `base`, in
// (0, 1): the digits of `index` in that base, mirrored at the point.
float Halton(uint32_t index, uint32_t base) {
  float result = 0.0f;
  float fraction = 1.0f;
  while (index > 0) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

// Returns the side of the render target for an output side of `output`
// pixels (not 0) at `ratio`, which lies in [`min_scale`, `max_scale`]: the
// nearest pixel count, corrected when rounding took output / side out of
// that range.
uint32_t RenderSide(uint32_t output, float ratio, float min_scale,
                    float max_scale) {
  const auto size = static_cast<float>(output);
  auto side = static_cast<uint32_t>(std::lround(size / ratio));
  side = std::clamp(side, uint32_t{1}, output);
  // Never larger than the output, whatever the range says.
  while (side < output && size / static_cast<float>(side) > max_scale) {
    ++side;
  }
  while (side > 1 && size / static_cast<float>(side) < min_scale) {
    --side;
  }
  return side;
}

}  // namespace

const char* UpscalingName(Upscaling mode) {
  switch (mode) {
    case Upscaling::kOff:
      return "Off";
    case Upscaling::kNative:
      return "Native (TAA)";
    case Upscaling::kQuality:
      return "Quality (1.5x)";
    case Upscaling::kBalanced:
      return "Balanced (1.7x)";
    case Upscaling::kPerformance:
      return "Performance (2x)";
  }
  return "Off";
}

float UpscalingRatio(Upscaling mode) {
  switch (mode) {
    case Upscaling::kOff:
    case Upscaling::kNative:
      return 1.0f;
    case Upscaling::kQuality:
      return 1.5f;
    case Upscaling::kBalanced:
      return 1.7f;
    case Upscaling::kPerformance:
      return 2.0f;
  }
  return 1.0f;
}

core::Extent2D RenderExtent(core::Extent2D output, Upscaling mode,
                            float min_scale, float max_scale) {
  const float nominal = UpscalingRatio(mode);
  if (nominal == 1.0f || output.width == 0 || output.height == 0) {
    return output;
  }
  // A NaN bound fails its comparison and falls back.
  const float lowest = min_scale > 0.0f ? min_scale : 1.0f;
  const float highest = max_scale >= lowest ? max_scale : lowest;
  const float ratio = std::clamp(nominal, lowest, highest);
  return {.width = RenderSide(output.width, ratio, lowest, highest),
          .height = RenderSide(output.height, ratio, lowest, highest)};
}

uint32_t JitterPhaseCount(core::Extent2D render, core::Extent2D output) {
  if (render.width == 0 || output.width <= render.width) {
    return kJitterBasePhaseCount;
  }
  const float ratio =
      static_cast<float>(output.width) / static_cast<float>(render.width);
  // The epsilon keeps an exact ratio (2: 32) from rounding up a phase.
  const float phases =
      static_cast<float>(kJitterBasePhaseCount) * ratio * ratio;
  return static_cast<uint32_t>(std::ceil(phases - 1.0e-3f));
}

glm::vec2 JitterOffset(uint64_t frame, uint32_t phase_count) {
  const uint32_t count = std::max(phase_count, uint32_t{1});
  // Halton index 0 is the corner (0, 0): start from 1.
  const auto index = static_cast<uint32_t>(frame % count) + 1;
  return {Halton(index, 2) - 0.5f, Halton(index, 3) - 0.5f};
}

glm::mat4 JitterProjection(const glm::mat4& projection,
                           glm::vec2 jitter_pixels, core::Extent2D render) {
  // One pixel is 2 / size in NDC, whose y points up. Adding a multiple of
  // the w row to the x and y rows shifts the point after the divide.
  const float shift_x =
      2.0f * jitter_pixels.x / static_cast<float>(render.width);
  const float shift_y =
      -2.0f * jitter_pixels.y / static_cast<float>(render.height);
  glm::mat4 jittered = projection;
  for (glm::length_t column = 0; column < 4; ++column) {
    jittered[column][0] += shift_x * projection[column][3];
    jittered[column][1] += shift_y * projection[column][3];
  }
  return jittered;
}

float TextureLodBias(core::Extent2D render, core::Extent2D output) {
  if (render.width == 0 || output.width == 0) {
    return 0.0f;
  }
  const float bias = std::log2(static_cast<float>(render.width) /
                               static_cast<float>(output.width));
  return std::clamp(bias, kMinTextureLodBias, 0.0f);
}

size_t TextureLodBiasStep(float lod_bias) {
  if (!(lod_bias < 0.0f)) {
    return 0;  // Also NaN.
  }
  const float steps =
      std::round(std::max(lod_bias, kMinTextureLodBias) *
                 -static_cast<float>(kTextureLodBiasStepsPerLevel));
  return static_cast<size_t>(steps);
}

glm::vec2 MotionVector(const glm::vec4& clip, const glm::vec4& previous_clip) {
  if (!(clip.w > kMotionVectorMinClipW) ||
      !(previous_clip.w > kMotionVectorMinClipW)) {
    return glm::vec2(0.0f);
  }
  const glm::vec2 ndc = glm::vec2(clip) / clip.w;
  const glm::vec2 previous_ndc = glm::vec2(previous_clip) / previous_clip.w;
  const glm::vec2 motion = (previous_ndc - ndc) * glm::vec2(0.5f, -0.5f);
  return std::isfinite(motion.x) && std::isfinite(motion.y) ? motion
                                                            : glm::vec2(0.0f);
}

}  // namespace blue::render
