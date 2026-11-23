#include "render/frame_uniforms.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/matrix.hpp>
#include <glm/vec2.hpp>

#include "camera/frustum.h"
#include "render/motion_blur_math.h"

namespace blue::render {

namespace {

static_assert(lighting::kCascadeCount == 4,
              "The cascade vec4s in FrameUniforms hold exactly four values");
static_assert(offsetof(FrameUniforms, view_proj) == 0);
static_assert(offsetof(FrameUniforms, view) == 64);
static_assert(offsetof(FrameUniforms, projection) == 128);
static_assert(offsetof(FrameUniforms, inverse_projection) == 192);
static_assert(offsetof(FrameUniforms, inverse_view_proj) == 256);
static_assert(offsetof(FrameUniforms, camera_position) == 320);
static_assert(offsetof(FrameUniforms, ambient_color) == 336);
static_assert(offsetof(FrameUniforms, sun_direction) == 352);
static_assert(offsetof(FrameUniforms, sun_color) == 368);
static_assert(offsetof(FrameUniforms, cascade_view_projection) == 384);
static_assert(offsetof(FrameUniforms, cascade_split_far) == 640);
static_assert(offsetof(FrameUniforms, cascade_texel_world_size) == 656);
static_assert(offsetof(FrameUniforms, shadow_params) == 672);
static_assert(offsetof(FrameUniforms, screen) == 688);
static_assert(offsetof(FrameUniforms, irradiance_sh) == 704);
static_assert(offsetof(FrameUniforms, environment_params) == 848);
static_assert(offsetof(FrameUniforms, point_light_count) == 864);
static_assert(offsetof(FrameUniforms, point_lights) == 880);
static_assert(sizeof(GpuPointLight) == 32);
static_assert(offsetof(FrameUniforms, previous_view_proj) ==
              880 + 32 * kMaxPointLights);
static_assert(offsetof(FrameUniforms, motion_blur) ==
              944 + 32 * kMaxPointLights);
static_assert(sizeof(FrameUniforms) == 960 + 32 * kMaxPointLights);

// Shadow lookups move along the surface normal by this many shadow texels.
constexpr float kShadowNormalOffsetTexels = 1.5f;

// Returns how strongly `light` lights the point `eye` (its irradiance
// there, up to a constant): the priority used to drop lights beyond
// kMaxPointLights.
float LightImportance(const PointLight& light, const glm::vec3& eye) {
  const glm::vec3 offset = light.position - eye;
  const float brightness =
      light.intensity *
      glm::max(light.color.r, glm::max(light.color.g, light.color.b));
  return brightness / glm::max(glm::dot(offset, offset), 1.0f);
}

// Fills the point-light fields of `uniforms` from `lights`: those
// outside `frustum` are culled, then the kMaxPointLights most important
// for `eye` are kept (in their original order when all fit).
void FillPointLights(std::span<const PointLight> lights,
                     const camera::Frustum& frustum, const glm::vec3& eye,
                     FrameUniforms& uniforms) {
  std::array<float, kMaxPointLights> importance{};
  uint32_t count = 0;
  for (const PointLight& light : lights) {
    if (!(light.intensity > 0.0f)) {
      continue;
    }
    if (light.range > 0.0f) {
      const glm::vec3 reach(light.range);
      if (!frustum.Intersects(
              {.min = light.position - reach, .max = light.position + reach})) {
        continue;
      }
    }
    const float score = LightImportance(light, eye);
    uint32_t slot = count;
    if (count == kMaxPointLights) {
      // Full: replace the least important light if this one beats it.
      const auto weakest = std::ranges::min_element(importance);
      if (*weakest >= score) {
        continue;
      }
      slot = static_cast<uint32_t>(weakest - importance.begin());
    } else {
      ++count;
    }
    importance[slot] = score;
    const float inverse_range_sq =
        light.range > 0.0f ? 1.0f / (light.range * light.range) : 0.0f;
    uniforms.point_lights[slot] = {
        .position_inverse_range_sq =
            glm::vec4(light.position, inverse_range_sq),
        .radiance = glm::vec4(light.color * light.intensity, 0.0f),
    };
  }
  uniforms.point_light_count = glm::uvec4(count, 0, 0, 0);
}

// Returns FrameUniforms::motion_blur for `view` on a target of `extent`.
glm::vec4 MotionBlurUniform(const SceneView& view, core::Extent2D extent) {
  const float scale =
      view.post.weapon_motion_blur_enabled
          ? MotionBlurScale(view.post.weapon_motion_blur_strength,
                            view.frame_seconds)
          : 0.0f;
  const auto max_radius =
      static_cast<float>(MotionBlurMaxRadius(extent.height));
  return {scale, 2.0f * max_radius, max_radius, 0.0f};
}

}  // namespace

FrameUniforms MakeFrameUniforms(const SceneView& view,
                                std::span<const lighting::Cascade> cascades,
                                bool shadows, core::Extent2D extent) {
  const glm::mat4 view_proj = view.camera.projection * view.camera.view;
  const glm::vec2 size(static_cast<float>(extent.width),
                       static_cast<float>(extent.height));
  FrameUniforms uniforms{
      .view_proj = view_proj,
      .view = view.camera.view,
      .projection = view.camera.projection,
      .inverse_projection = glm::inverse(view.camera.projection),
      .inverse_view_proj = glm::inverse(view_proj),
      .camera_position = glm::vec4(view.camera.position, 1.0f),
      .ambient_color = glm::vec4(view.ambient, 0.0f),
      .sun_direction = glm::vec4(view.sun.direction, 0.0f),
      .sun_color = glm::vec4(view.sun.color * view.sun.intensity, 0.0f),
      .cascade_view_projection = {},
      .cascade_split_far = glm::vec4(0.0f),
      .cascade_texel_world_size = glm::vec4(0.0f),
      .shadow_params =
          glm::vec4(shadows ? 1.0f : 0.0f, view.cascade_debug ? 1.0f : 0.0f,
                    lighting::kShadowDistance, kShadowNormalOffsetTexels),
      .screen = glm::vec4(size, 1.0f / glm::max(size, glm::vec2(1.0f))),
      .irradiance_sh = {},
      .environment_params = glm::vec4(0.0f),
      .point_light_count = glm::uvec4(0),
      .point_lights = {},
      .previous_view_proj = view.camera.previous_view_projection,
      .motion_blur = MotionBlurUniform(view, extent),
  };
  FillPointLights(view.point_lights, camera::Frustum(view_proj),
                  view.camera.position, uniforms);
  if (shadows) {
    for (size_t i = 0; i < cascades.size() && i < lighting::kCascadeCount;
         ++i) {
      const auto component = static_cast<glm::length_t>(i);
      uniforms.cascade_view_projection[i] = cascades[i].view_projection;
      uniforms.cascade_split_far[component] = cascades[i].split_far;
      uniforms.cascade_texel_world_size[component] =
          cascades[i].texel_world_size;
    }
  }
  return uniforms;
}

}  // namespace blue::render
