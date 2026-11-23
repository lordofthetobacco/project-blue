#ifndef BLUE_RENDER_TEMPORAL_MATH_H_
#define BLUE_RENDER_TEMPORAL_MATH_H_

#include <cstddef>
#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "core/extent.h"
#include "render/scene_view.h"

// The math of temporal upscaling and anti-aliasing (PostSettings::upscaling,
// the MetalFX temporal scaler): the resolution the scene is rendered at, the
// sub-pixel jitter of each frame and the projection that applies it, the
// texture LOD bias that goes with a lower render resolution, and the motion
// vector of a surface point. Pure functions; the renderer and the shaders
// feed the scaler with them.
//
// Conventions. The scene is rendered at the render extent and scaled to the
// output extent; "ratio" is output / render per axis (1 for Upscaling::kNative,
// where the scaler only anti-aliases). Pixel coordinates are Metal's: x to
// the right, y down, the origin in the top-left corner. Everything that is
// compared between frames (motion vectors) uses unjittered projections.
namespace blue::render {

// The jitter sequence is at least this long (the length at ratio 1); it
// grows with the square of the ratio, so that every output pixel is still
// covered by about this many samples.
inline constexpr uint32_t kJitterBasePhaseCount = 8;
// The texture LOD bias is never below this (a ratio of 4).
inline constexpr float kMinTextureLodBias = -2.0f;
// Texture LOD biases are applied in steps of 1 / this of a mip level (a
// sampler exists per step, render::Samplers); a fraction of a level that
// small is not visible.
inline constexpr int kTextureLodBiasStepsPerLevel = 4;
// The steps from 0 down to kMinTextureLodBias.
inline constexpr size_t kTextureLodBiasStepCount = 9;
// Clip-space w at or below which a point counts as behind the camera and
// has no motion vector.
inline constexpr float kMotionVectorMinClipW = 1.0e-6f;

// Returns true when `mode` runs the temporal scaler (every mode but kOff).
[[nodiscard]] constexpr bool UsesTemporalScaler(Upscaling mode) {
  return mode != Upscaling::kOff;
}

// Returns true when the present pass applies FXAA: it is enabled and the
// temporal scaler, which anti-aliases by itself, is off.
[[nodiscard]] constexpr bool FxaaActive(const PostSettings& post) {
  return post.fxaa_enabled && !UsesTemporalScaler(post.upscaling);
}

// Returns the name of `mode` for the UI, with its ratio: "Off",
// "Native (TAA)", "Quality (1.5x)", "Balanced (1.7x)", "Performance (2x)".
// Never null.
[[nodiscard]] const char* UpscalingName(Upscaling mode);

// Returns the nominal ratio of `mode`: 1 for kOff and kNative, 1.5 for
// kQuality, 1.7 for kBalanced, 2 for kPerformance.
[[nodiscard]] float UpscalingRatio(Upscaling mode);

// Returns the extent the scene is rendered at for an output of `output`
// pixels: `output` itself for kOff and kNative, else each side divided by
// the mode's ratio and rounded to the nearest pixel (at least 1). The ratio
// is first clamped to what the scaler supports, [`min_scale`, `max_scale`]
// (MTLFXTemporalScalerDescriptor's supportedInputContentMinScaleForDevice
// and ...MaxScaleForDevice), and after rounding neither side's ratio lies
// outside that range, as far as whole pixels and a render target no larger
// than the output allow. A `min_scale` that is not positive counts as 1, a
// `max_scale` below the minimum as the minimum.
[[nodiscard]] core::Extent2D RenderExtent(core::Extent2D output,
                                          Upscaling mode, float min_scale,
                                          float max_scale);

// Returns the length of the jitter sequence for rendering at `render` and
// scaling to `output`: kJitterBasePhaseCount times the square of the ratio,
// rounded up (8 at ratio 1, 18 at 1.5, 24 at 1.7, 32 at 2). Never 0; empty
// extents give kJitterBasePhaseCount.
[[nodiscard]] uint32_t JitterPhaseCount(core::Extent2D render,
                                        core::Extent2D output);

// Returns the jitter of frame number `frame` in a sequence of `phase_count`
// frames (0 counts as 1): where in its pixel each pixel of the render target
// is sampled, as an offset from the pixel centre in render-target pixels,
// each component in (-0.5, 0.5). Halton points (bases 2 and 3), which cover
// the pixel evenly after any number of frames; the sequence repeats every
// `phase_count` frames. Hand the same value to JitterProjection() and, as
// it is, to the scaler (MTLFXTemporalScaler jitterOffsetX / Y = +x / +y:
// measured, a static camera converges with that sign and shimmers with the
// opposite one).
[[nodiscard]] glm::vec2 JitterOffset(uint64_t frame, uint32_t phase_count);

// Returns `projection` (view to clip space) with the jitter `jitter_pixels`
// applied for a render target of `render` pixels: every point lands
// `jitter_pixels` further right and down on the target than with
// `projection`; depth and w are unchanged. So the centre of a pixel now sees
// what lay -`jitter_pixels` away from it. `render` must not be empty.
[[nodiscard]] glm::mat4 JitterProjection(const glm::mat4& projection,
                                         glm::vec2 jitter_pixels,
                                         core::Extent2D render);

// Returns the LOD bias of material texture lookups when the scene is
// rendered at `render` and shown at `output`: log2 of render / output width,
// so textures are as sharp as if rendered at the output resolution. At most
// 0 (never blurrier than unbiased), at least kMinTextureLodBias; 0 for
// empty extents.
[[nodiscard]] float TextureLodBias(core::Extent2D render,
                                   core::Extent2D output);

// Returns the step, in [0, kTextureLodBiasStepCount), nearest to
// `lod_bias`: step i is the bias -i / kTextureLodBiasStepsPerLevel.
// Positive biases and NaN give 0, biases below kMinTextureLodBias the last
// step.
[[nodiscard]] size_t TextureLodBiasStep(float lod_bias);
// Returns the bias of `step`, which must be below kTextureLodBiasStepCount.
[[nodiscard]] constexpr float TextureLodBiasOfStep(size_t step) {
  return -static_cast<float>(step) /
         static_cast<float>(kTextureLodBiasStepsPerLevel);
}
static_assert(TextureLodBiasOfStep(kTextureLodBiasStepCount - 1) ==
              kMinTextureLodBias);

// Returns the motion vector of a surface point that is at `clip` this frame
// and was at `previous_clip` in the previous one (both in clip space, both
// from unjittered view-projections): the offset from where it is now to
// where it was, in UV units of the target (x right, y down, 1 = the whole
// width or height). Times the render extent it is the offset in pixels the
// temporal scaler expects (motionVectorScaleX / Y = render width / height):
// a point that moved 10 pixels right and down has (-10, -10). Zero when
// either point is not in front of the camera (w <= kMotionVectorMinClipW)
// or the result is not finite.
//
// A copy of MotionVector() in the shader that writes the scene's motion
// vectors, expression for expression: keep the two in sync.
[[nodiscard]] glm::vec2 MotionVector(const glm::vec4& clip,
                                     const glm::vec4& previous_clip);

}  // namespace blue::render

#endif  // BLUE_RENDER_TEMPORAL_MATH_H_
