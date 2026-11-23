#ifndef BLUE_LIGHTING_ENVIRONMENT_H_
#define BLUE_LIGHTING_ENVIRONMENT_H_

#include <array>
#include <limits>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "asset/image_decoder.h"

// CPU-side image-based lighting math: equirectangular environment maps,
// spherical-harmonics diffuse irradiance and the split-sum BRDF lookup
// table.
//
// Equirect convention (matches the shaders): world space is right-handed,
// Y up. For a unit direction d,
//   u = 0.5 + atan2(d.x, -d.z) / (2 pi)     (u = 0.5 looks down -Z)
//   v = acos(clamp(d.y, -1, 1)) / pi        (v = 0 is straight up, +Y)
// with (0, 0) the top-left texel corner of the image, as in asset::Vertex.
// Rotating the environment by `r` radians about +Y means sampling with
// direction RotateY(d, -r).

namespace blue::lighting {

// Returns the image coordinate of unit direction `direction`.
[[nodiscard]] glm::vec2 DirectionToEquirectUv(glm::vec3 direction);

// Returns the unit direction of equirect image coordinate `uv`.
[[nodiscard]] glm::vec3 EquirectUvToDirection(glm::vec2 uv);

// Returns the bilinearly filtered radiance of `image` at `uv`: u wraps
// around, v is clamped. `image` must not be empty.
[[nodiscard]] glm::vec3 SampleEquirect(const asset::ImageDataF& image,
                                       glm::vec2 uv);

// Nine SH coefficients (bands 0..2) of irradiance, RGB each, with the cosine
// lobe convolution and the basis constants folded in, so that the
// irradiance E(n) for unit normal n is (as in the shaders):
//
//   E = c[0]
//     + c[1] * n.y + c[2] * n.z + c[3] * n.x
//     + c[4] * n.x * n.y + c[5] * n.y * n.z + c[6] * (3 n.z^2 - 1)
//     + c[7] * n.x * n.z + c[8] * (n.x^2 - n.y^2)
//
// in world axes (Ramamoorthi & Hanrahan 2001 basis order L00, L1-1, L10,
// L11, L2-2, L2-1, L20, L21, L22). Diffuse radiance leaving a Lambert surface
// is albedo / pi * E; a constant environment of radiance L gives E = pi * L.
struct IrradianceSh9 {
  std::array<glm::vec3, 9> coefficients{};
};

// Returns the irradiance at unit normal `normal` from `sh` (the formula
// above; the shader evaluates the same).
[[nodiscard]] glm::vec3 EvaluateIrradiance(const IrradianceSh9& sh,
                                           glm::vec3 normal);

// Projects equirect radiance `image` onto SH9 and convolves it with the
// clamped cosine lobe. Every texel is weighted by its solid angle. Texels
// brighter than `clamp_luminance` are scaled down to it first (see
// SunEstimate), then images wider than `max_width` are box-downsampled to
// bound the cost.
[[nodiscard]] IrradianceSh9 ComputeIrradianceSh9(
    const asset::ImageDataF& image, int max_width = 256,
    float clamp_luminance = std::numeric_limits<float>::infinity());

// Split-sum BRDF lookup table: for texel (x, y), NdotV = (x + 0.5) / size
// and roughness = (y + 0.5) / size (perceptual; row 0 = smoothest). Each
// texel holds (scale, bias) so that the specular IBL term is
//   prefiltered * (F0 * scale + F90 * bias),
// from importance-sampled GGX with height-correlated Smith visibility.
// For multi-scatter energy compensation (Fdez-Aguera 2019) the single-scatter
// directional albedo for F0 = F90 = 1 is E = scale + bias; the third channel
// is therefore not stored. `pixels` holds size * size * 2 floats, rows top
// to bottom.
struct BrdfLut {
  int size = 0;
  std::vector<float> pixels;
};

// Returns the BRDF LUT with `size` x `size` texels and `sample_count`
// importance samples per texel.
[[nodiscard]] BrdfLut GenerateBrdfLut(int size, int sample_count = 256);

// Returns the unit direction of the brightest region of equirect `image`
// (luminance-weighted centroid of the brightest texels after a downsample),
// for aligning the sun with an HDRI. Points from the scene towards the light
// source, i.e. the opposite of scene::SunLight::direction.
[[nodiscard]] glm::vec3 BrightestDirection(const asset::ImageDataF& image);

// The sun of an HDRI, found so the scene's analytic sun (which casts
// shadows) can replace it: lighting from the environment uses the HDRI with
// texels above `clamp_luminance` scaled down to it (luminance-preserving
// hue), and the analytic sun supplies what was removed.
struct SunEstimate {
  // False when nothing stands out from the sky (overcast or indoor HDRIs);
  // then direction, color and intensity are meaningless, and
  // clamp_luminance is still the threshold that was used.
  bool found = false;
  // Direction the light travels, unit (as scene::SunLight::direction).
  glm::vec3 direction{0.0f, -1.0f, 0.0f};
  // Linear RGB with the largest component 1.
  glm::vec3 color{1.0f};
  // As scene::SunLight::intensity: color * intensity is the radiance a white
  // Lambertian surface facing the sun reflects, i.e. the removed irradiance
  // over pi. Multiply by the environment intensity used for rendering.
  float intensity = 0.0f;
  // Texels with a larger luminance belong to the sun.
  float clamp_luminance = std::numeric_limits<float>::infinity();
};

// Finds the sun of equirect radiance `equirect`: texels brighter than
// max(20, 50 x the median luminance) are the sun; their luminance above
// that threshold is removed light. Linear in the image size (about 30 ms
// for 2048 x 1024 in a Debug build).
[[nodiscard]] SunEstimate EstimateSun(const asset::ImageDataF& equirect);

// Returns a neutral procedural environment for scenes without an HDRI: a
// soft blue-grey sky brightening towards the horizon, and a darker grey-brown
// ground below it with a smooth transition. Linear radiance; the sky is
// roughly 1.0 at the horizon, the ground about 0.15.
[[nodiscard]] asset::ImageDataF MakeDefaultEnvironment(int width, int height);

}  // namespace blue::lighting

#endif  // BLUE_LIGHTING_ENVIRONMENT_H_
