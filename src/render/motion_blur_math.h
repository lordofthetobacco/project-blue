#ifndef BLUE_RENDER_MOTION_BLUR_MATH_H_
#define BLUE_RENDER_MOTION_BLUR_MATH_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include <glm/geometric.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

// The math of the weapon motion blur (render::MotionBlurPass): how a surface
// point's motion on screen becomes the blur vector it is smeared along.
//
// ScreenVelocity() and BlurVector() are copies of the functions of the same
// names in shaders/motion_blur.glsl, expression for expression: keep the two
// in sync. The shader runs them per pixel; this copy lets tests check them.
// The other functions here are CPU-only: the shader's inputs and the area
// the pass works on.
//
// The blur vector b of a pixel is where its surface point moves on screen
// during the exposure, in pixels (x right, y down); the point is smeared over
// the segment from -b / 2 to +b / 2 around it. Its length is the screen motion
// over kMotionBlurReferenceSeconds at strength 1, whatever the frame rate.
namespace blue::render {

// The exposure the blur length corresponds to at strength 1: one frame at
// 60 Hz.
inline constexpr float kMotionBlurReferenceSeconds = 1.0f / 60.0f;
// Frames longer than this (a hitch, a load) blur nothing: their motion is
// not a straight line any more.
inline constexpr float kMotionBlurMaxFrameSeconds = 0.25f;
// The largest blur radius (half the blur length), as a fraction of the
// render target height.
inline constexpr float kMotionBlurRadiusFraction = 0.025f;
// Blur vectors shorter than this many pixels are dropped, so the idle bob
// never softens the weapon; from there to twice as long they ramp up to
// their full length, so blur does not pop in.
inline constexpr float kMotionBlurDeadZonePixels = 0.5f;
// Clip-space w at or below which a point counts as behind the camera.
inline constexpr float kMotionBlurMinClipW = 1.0e-6f;

// Returns the largest blur radius in pixels for a render target `height`
// pixels tall (at least 1). It is also the tile size of the tile-max image
// (RenderTargets::motion_tiles()), so a pixel is only ever reached by blur
// from its own tile and the eight around it.
[[nodiscard]] inline int MotionBlurMaxRadius(uint32_t height) {
  return std::max(1, static_cast<int>(std::lround(static_cast<float>(height) *
                                                  kMotionBlurRadiusFraction)));
}

// Returns the factor that turns a frame's screen motion (pixels since the
// previous frame) into the blur vector: strength * reference exposure /
// frame duration. 0 (no blur) for a non-positive strength, a frame of
// zero, negative, non-finite or more than kMotionBlurMaxFrameSeconds
// duration.
[[nodiscard]] inline float MotionBlurScale(float strength,
                                           float frame_seconds) {
  if (!(strength > 0.0f) || !(frame_seconds > 0.0f) ||
      !(frame_seconds <= kMotionBlurMaxFrameSeconds)) {
    return 0.0f;
  }
  return strength * kMotionBlurReferenceSeconds / frame_seconds;
}

// motion_blur.glsl: returns how far a surface point moved on screen since the
// previous frame, in pixels (x right, y down), from its clip-space position
// this frame and last frame, on a target of `extent` pixels whose clip-space
// y points up (the scene passes' flipped viewport). Zero when either
// position is behind its camera (w <= kMotionBlurMinClipW) or the result is
// not finite.
[[nodiscard]] inline glm::vec2 ScreenVelocity(const glm::vec4& current_clip,
                                              const glm::vec4& previous_clip,
                                              const glm::vec2& extent) {
  if (!(current_clip.w > kMotionBlurMinClipW) ||
      !(previous_clip.w > kMotionBlurMinClipW)) {
    return glm::vec2(0.0f);
  }
  const glm::vec2 delta = (glm::vec2(current_clip) / current_clip.w -
                           glm::vec2(previous_clip) / previous_clip.w) *
                          0.5f * extent;
  const glm::vec2 velocity(delta.x, -delta.y);
  return std::isfinite(velocity.x) && std::isfinite(velocity.y)
             ? velocity
             : glm::vec2(0.0f);
}

// motion_blur.glsl: returns the blur vector of a point moving by `velocity`
// pixels per frame: velocity * scale (MotionBlurScale()), with the dead zone
// (kMotionBlurDeadZonePixels) applied and its length clamped to
// `max_length` (> 0, twice MotionBlurMaxRadius()). Zero when the length is
// not finite.
[[nodiscard]] inline glm::vec2 BlurVector(const glm::vec2& velocity,
                                          float scale, float max_length) {
  const glm::vec2 blur = velocity * scale;
  const float length = glm::length(blur);
  if (!(length >= kMotionBlurDeadZonePixels) || std::isinf(length)) {
    return glm::vec2(0.0f);
  }
  const float ramped = length < 2.0f * kMotionBlurDeadZonePixels
                           ? (length - kMotionBlurDeadZonePixels) * 2.0f
                           : length;
  return blur * (std::min(ramped, max_length) / length);
}

// The screen rectangle covered by a projected box, in pixels (x right, y
// down; not clamped to the target).
struct ScreenBounds {
  glm::vec2 min{0.0f};
  glm::vec2 max{0.0f};
};

// Returns the pixel bounds, on a target of `extent` pixels, of the part of the
// world-space box [box_min, box_max] that the rasterizer can draw through
// `view_proj` (clip-space y up, Vulkan depth 0 <= z <= w): the box's faces are
// clipped in clip space against z >= 0, z <= w (the near plane of the
// reversed-Z projection) and w >= kMotionBlurMinClipW before projecting, so
// a box reaching behind the camera is still bounded by its visible part.
// Every point of the box that can be rasterized projects inside the result.
// nullopt when no part of the box can be drawn. Not clamped to the target and
// possibly non-finite for degenerate matrices: callers check.
[[nodiscard]] std::optional<ScreenBounds> ProjectBoxToScreen(
    const glm::vec3& box_min, const glm::vec3& box_max,
    const glm::mat4& view_proj, const glm::vec2& extent);

}  // namespace blue::render

#endif  // BLUE_RENDER_MOTION_BLUR_MATH_H_
