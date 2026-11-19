#ifndef BLUE_RENDER_BRDF_REFERENCE_H_
#define BLUE_RENDER_BRDF_REFERENCE_H_

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

// A CPU copy of the shading math in shaders/pbr.h, shaders/sanitize.h and
// shaders/environment.h (ImportanceSampleGgx), for tests that sweep it over
// every legal input and check that it stays finite. Functions have the
// shader names and the same expressions, line for line: keep the two in
// sync. Not used for rendering.
namespace blue::render::brdf_reference {

inline constexpr float kPi = 3.14159265359f;
inline constexpr float kMinPerceptualRoughness = 0.045f;
inline constexpr float kDielectricF0 = 0.04f;
inline constexpr float kMaxSceneRadiance = 4096.0f;

// sanitize.h: NaN -> 0, then clamped to [0, max_value].
inline float SanitizeColor(float value, float max_value) {
  return std::isnan(value) ? 0.0f : std::clamp(value, 0.0f, max_value);
}

inline glm::vec3 SanitizeRadiance(const glm::vec3& color) {
  return {SanitizeColor(color.x, kMaxSceneRadiance),
          SanitizeColor(color.y, kMaxSceneRadiance),
          SanitizeColor(color.z, kMaxSceneRadiance)};
}

inline glm::vec3 SafeNormalize(const glm::vec3& v, const glm::vec3& fallback) {
  const float length_sq = glm::dot(v, v);
  return length_sq > 1.0e-20f && !std::isinf(length_sq)
             ? v / std::sqrt(length_sq)
             : fallback;
}

inline float DistributionGgx(float n_dot_h, float alpha) {
  const float a2 = alpha * alpha;
  const float d = std::max((n_dot_h * a2 - n_dot_h) * n_dot_h + 1.0f, a2);
  return a2 / (kPi * d * d);
}

inline float VisibilitySmithGgxCorrelated(float n_dot_v, float n_dot_l,
                                          float alpha) {
  const float a2 = alpha * alpha;
  const float ggx_v =
      n_dot_l * std::sqrt(std::max(n_dot_v * n_dot_v * (1.0f - a2) + a2, 0.0f));
  const float ggx_l =
      n_dot_v * std::sqrt(std::max(n_dot_l * n_dot_l * (1.0f - a2) + a2, 0.0f));
  return 0.5f / std::max(ggx_v + ggx_l, 1.0e-7f);
}

inline glm::vec3 FresnelSchlick(const glm::vec3& f0, float v_dot_h) {
  const float f = std::pow(std::clamp(1.0f - v_dot_h, 0.0f, 1.0f), 5.0f);
  return f + f0 * (1.0f - f);
}

// The parts of the GLSL Surface that the BRDF reads.
struct Surface {
  glm::vec3 normal;
  glm::vec3 view;
  float n_dot_v;
  glm::vec3 diffuse_color;
  glm::vec3 f0;
  float roughness;
  float alpha;
};

// MakeSurface() without the position: `view` is the (unnormalized) vector
// towards the camera.
inline Surface MakeSurface(const glm::vec3& normal, const glm::vec3& view,
                           const glm::vec3& base_color, float metallic,
                           float perceptual_roughness) {
  Surface s;
  s.normal = normal;
  s.view = SafeNormalize(view, normal);
  s.n_dot_v = std::clamp(glm::dot(normal, s.view), 1.0e-4f, 1.0f);
  s.diffuse_color = base_color * (1.0f - metallic);
  s.f0 = glm::clamp(glm::mix(glm::vec3(kDielectricF0), base_color, metallic),
                    0.0f, 1.0f);
  s.roughness = std::clamp(perceptual_roughness, kMinPerceptualRoughness, 1.0f);
  s.alpha = s.roughness * s.roughness;
  return s;
}

inline glm::vec3 DirectLight(const Surface& s, const glm::vec3& to_light,
                             const glm::vec3& intensity) {
  const float n_dot_l = std::min(glm::dot(s.normal, to_light), 1.0f);
  if (!(n_dot_l > 0.0f)) {
    return glm::vec3(0.0f);
  }
  const glm::vec3 halfway = SafeNormalize(s.view + to_light, s.normal);
  const float n_dot_h = std::clamp(glm::dot(s.normal, halfway), 0.0f, 1.0f);
  const float v_dot_h = std::clamp(glm::dot(s.view, halfway), 0.0f, 1.0f);

  const float d = DistributionGgx(n_dot_h, s.alpha);
  const float v = VisibilitySmithGgxCorrelated(s.n_dot_v, n_dot_l, s.alpha);
  const glm::vec3 f = FresnelSchlick(s.f0, v_dot_h);
  const glm::vec3 specular = d * v * f;
  const glm::vec3 diffuse = s.diffuse_color / kPi;
  return (diffuse + specular) * (kPi * intensity) * n_dot_l;
}

inline float SpecularOcclusion(float n_dot_v, float ao, float roughness) {
  const float base = std::max(n_dot_v + ao, 0.0f);
  return std::clamp(
      std::pow(base, std::exp2(-16.0f * roughness - 1.0f)) - 1.0f + ao, 0.0f,
      1.0f);
}

// The weights AmbientLight() gives the prefiltered radiance (x) and the
// irradiance (y, z: specular multiple scattering, diffuse), for a grey f0 and
// diffuse colour, from a LUT texel `lut` (already through EnvironmentBrdf).
inline glm::vec3 AmbientWeights(const Surface& s, const glm::vec2& lut) {
  const glm::vec3 single_scatter = s.f0 * lut.x + lut.y;
  const float single_energy = std::min(lut.x + lut.y, 1.0f);
  const float multi_energy = 1.0f - single_energy;
  const glm::vec3 average_fresnel = s.f0 + (1.0f - s.f0) / 21.0f;
  const glm::vec3 multi_scatter =
      single_scatter * average_fresnel /
      glm::max(1.0f - multi_energy * average_fresnel, glm::vec3(1.0e-4f));
  const glm::vec3 diffuse_weight =
      s.diffuse_color *
      glm::max(1.0f - single_scatter - multi_scatter * multi_energy,
               glm::vec3(0.0f));
  return {single_scatter.x, (multi_scatter * multi_energy).x, diffuse_weight.x};
}

// Division as the GPU may do it (x * rcp(y), which Vulkan lets be 2.5 ulp
// off): tests pass one that returns 1 + ulp for equal operands.
using Divide = float (*)(float numerator, float denominator);

// environment.h: a GGX half vector around +Z for the uniform sample `xi`.
inline glm::vec3 ImportanceSampleGgx(const glm::vec2& xi, float alpha,
                                     Divide quotient) {
  const float phi = 2.0f * kPi * xi.x;
  const float denominator =
      std::max(1.0f + (alpha * alpha - 1.0f) * xi.y, 1.0e-20f);
  const float cos_theta_sq =
      std::clamp(quotient(1.0f - xi.y, denominator), 0.0f, 1.0f);
  const float cos_theta = std::sqrt(cos_theta_sq);
  const float sin_theta = std::sqrt(std::max(1.0f - cos_theta_sq, 0.0f));
  return {sin_theta * std::cos(phi), sin_theta * std::sin(phi), cos_theta};
}

}  // namespace blue::render::brdf_reference

#endif  // BLUE_RENDER_BRDF_REFERENCE_H_
