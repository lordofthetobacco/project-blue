#include "lighting/environment.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

namespace blue::lighting {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr double kPiD = std::numbers::pi;

// Width the brightest-direction search downsamples to.
constexpr int kBrightestSearchWidth = 128;
// Texels at least this fraction of the brightest one count as "the light".
constexpr float kBrightestFraction = 0.9f;

// EstimateSun(): the sky's median luminance is measured on a copy this wide.
constexpr int kSunSearchWidth = 128;
// A texel is part of the sun when it is this many times brighter than the
// median sky, and at least kMinSunLuminance. Daylight HDRIs have skies of
// ~1-5 and sun discs of 1e4-1e5, so the exact factor hardly matters; it
// must stay above the circumsolar glow of hazy skies.
constexpr float kSunToSkyLuminanceRatio = 50.0f;
constexpr float kMinSunLuminance = 20.0f;

// SH basis constants, Y_lm = K * polynomial (see IrradianceSh9).
constexpr std::array<double, 9> kShBasis = {0.282095, 0.488603, 0.488603,
                                            0.488603, 1.092548, 1.092548,
                                            0.315392, 1.092548, 0.546274};
// Clamped-cosine convolution per band: pi, 2 pi / 3, pi / 4.
constexpr std::array<double, 9> kCosineLobe = {
    kPiD,       2.0 * kPiD / 3.0, 2.0 * kPiD / 3.0, 2.0 * kPiD / 3.0,
    kPiD / 4.0, kPiD / 4.0,       kPiD / 4.0,       kPiD / 4.0,
    kPiD / 4.0};

// Returns the nine SH polynomials (without constants) at unit `n`.
std::array<float, 9> ShPolynomials(glm::vec3 n) {
  return {1.0f,
          n.y,
          n.z,
          n.x,
          n.x * n.y,
          n.y * n.z,
          3.0f * n.z * n.z - 1.0f,
          n.x * n.z,
          n.x * n.x - n.y * n.y};
}

glm::vec3 Texel(const asset::ImageDataF& image, int x, int y) {
  const size_t offset =
      (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
       static_cast<size_t>(x)) *
      3;
  return {image.pixels[offset], image.pixels[offset + 1],
          image.pixels[offset + 2]};
}

// Rec. 709 luminance weights.
constexpr float kLumaR = 0.2126f;
constexpr float kLumaG = 0.7152f;
constexpr float kLumaB = 0.0722f;

float Luminance(glm::vec3 rgb) {
  return kLumaR * rgb.r + kLumaG * rgb.g + kLumaB * rgb.b;
}

// Returns `image` with every texel clamped to `clamp_luminance`, then
// box-filtered down by an integer factor so that its width is at most
// `max_width`. Clamping first keeps a tiny, extremely bright sun from
// leaking into the downsampled texels.
asset::ImageDataF Downsample(
    const asset::ImageDataF& image, int max_width,
    float clamp_luminance = std::numeric_limits<float>::infinity()) {
  const int factor = std::max(
      1, (image.width + std::max(max_width, 1) - 1) / std::max(max_width, 1));
  if (factor == 1 && std::isinf(clamp_luminance)) {
    return image;
  }
  asset::ImageDataF result{.width = std::max(1, image.width / factor),
                           .height = std::max(1, image.height / factor)};
  result.pixels.assign(static_cast<size_t>(result.width) *
                           static_cast<size_t>(result.height) * 3,
                       0.0f);
  // Plain float arithmetic: this runs over every source texel, and glm's
  // operators are not inlined in Debug builds.
  const float* source = image.pixels.data();
  float* target = result.pixels.data();
  const auto width = static_cast<size_t>(image.width);
  for (int y = 0; y < std::min(result.height * factor, image.height); ++y) {
    float* row = target + static_cast<size_t>(y / factor) *
                              static_cast<size_t>(result.width) * 3;
    const float* texel = source + static_cast<size_t>(y) * width * 3;
    for (int x = 0; x < result.width * factor; ++x, texel += 3) {
      float scale = 1.0f;
      const float luminance =
          kLumaR * texel[0] + kLumaG * texel[1] + kLumaB * texel[2];
      if (luminance > clamp_luminance) {
        scale = clamp_luminance / luminance;
      }
      float* sum = row + static_cast<size_t>(x / factor) * 3;
      sum[0] += texel[0] * scale;
      sum[1] += texel[1] * scale;
      sum[2] += texel[2] * scale;
    }
  }
  const float normalization = 1.0f / static_cast<float>(factor * factor);
  for (float& value : result.pixels) {
    value *= normalization;
  }
  return result;
}

// Returns the solid angle of one texel in row `y` of an equirect image.
float TexelSolidAngle(int width, int height, int y) {
  const float theta =
      (static_cast<float>(y) + 0.5f) / static_cast<float>(height) * kPi;
  return (2.0f * kPi / static_cast<float>(width)) *
         (kPi / static_cast<float>(height)) * std::sin(theta);
}

// Returns the centre direction of texel (x, y).
glm::vec3 TexelDirection(int width, int height, int x, int y) {
  return EquirectUvToDirection(
      {(static_cast<float>(x) + 0.5f) / static_cast<float>(width),
       (static_cast<float>(y) + 0.5f) / static_cast<float>(height)});
}

// Returns the i-th point of the 2D Hammersley set of `count` points.
glm::vec2 Hammersley(uint32_t i, uint32_t count) {
  uint32_t bits = i;
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return {static_cast<float>(i) / static_cast<float>(count),
          static_cast<float>(bits) * 2.3283064365386963e-10f};
}

// Height-correlated Smith visibility V = G / (4 NdotL NdotV) for GGX with
// alpha `a` (Heitz 2014).
float SmithGgxCorrelated(float n_dot_v, float n_dot_l, float a) {
  const float a2 = a * a;
  const float lambda_v =
      n_dot_l * std::sqrt(std::max(n_dot_v * n_dot_v * (1.0f - a2) + a2, 0.0f));
  const float lambda_l =
      n_dot_v * std::sqrt(std::max(n_dot_l * n_dot_l * (1.0f - a2) + a2, 0.0f));
  return 0.5f / std::max(lambda_v + lambda_l, 1.0e-7f);
}

// Returns (scale, bias) of the split-sum for `n_dot_v` and perceptual
// `roughness`, in the tangent frame with N = +Z.
glm::vec2 IntegrateBrdf(float n_dot_v, float roughness, int sample_count) {
  const glm::vec3 view{std::sqrt(1.0f - n_dot_v * n_dot_v), 0.0f, n_dot_v};
  const float alpha = roughness * roughness;
  const auto count = static_cast<uint32_t>(sample_count);
  glm::vec2 sum(0.0f);
  for (uint32_t i = 0; i < count; ++i) {
    const glm::vec2 xi = Hammersley(i, count);
    // GGX importance sample of the half vector, as ImportanceSampleGgx in
    // shaders/environment.glsl: cos^2 is clamped to [0, 1] before both
    // square roots (for small alpha the quotient is x / x, which inexact
    // division can put above 1), and alpha 0 at xi.y = 1 is not 0 / 0.
    const float phi = 2.0f * kPi * xi.x;
    const float denominator =
        std::max(1.0f + (alpha * alpha - 1.0f) * xi.y, 1.0e-20f);
    const float cos_theta_sq =
        std::clamp((1.0f - xi.y) / denominator, 0.0f, 1.0f);
    const float cos_theta = std::sqrt(cos_theta_sq);
    const float sin_theta = std::sqrt(std::max(1.0f - cos_theta_sq, 0.0f));
    const glm::vec3 half{sin_theta * std::cos(phi), sin_theta * std::sin(phi),
                         cos_theta};
    const float v_dot_h = glm::dot(view, half);
    const glm::vec3 light = 2.0f * v_dot_h * half - view;
    const float n_dot_l = std::max(light.z, 0.0f);
    const float n_dot_h = std::max(half.z, 0.0f);
    if (n_dot_l <= 0.0f || v_dot_h <= 0.0f) {
      continue;
    }
    // BRDF * NdotL / pdf with pdf = D * NdotH / (4 VdotH); D cancels.
    const float weight = 4.0f * SmithGgxCorrelated(n_dot_v, n_dot_l, alpha) *
                         n_dot_l * v_dot_h / std::max(n_dot_h, 1.0e-7f);
    const float fresnel = std::pow(1.0f - v_dot_h, 5.0f);
    sum.x += (1.0f - fresnel) * weight;
    sum.y += fresnel * weight;
  }
  return sum / static_cast<float>(count);
}

}  // namespace

glm::vec2 DirectionToEquirectUv(glm::vec3 direction) {
  return {0.5f + std::atan2(direction.x, -direction.z) / (2.0f * kPi),
          std::acos(std::clamp(direction.y, -1.0f, 1.0f)) / kPi};
}

glm::vec3 EquirectUvToDirection(glm::vec2 uv) {
  const float phi = (uv.x - 0.5f) * 2.0f * kPi;
  const float theta = uv.y * kPi;
  return {std::sin(theta) * std::sin(phi), std::cos(theta),
          -std::sin(theta) * std::cos(phi)};
}

glm::vec3 SampleEquirect(const asset::ImageDataF& image, glm::vec2 uv) {
  const float x = uv.x * static_cast<float>(image.width) - 0.5f;
  const float y = std::clamp(uv.y * static_cast<float>(image.height) - 0.5f,
                             0.0f, static_cast<float>(image.height - 1));
  const float x_floor = std::floor(x);
  const float tx = x - x_floor;
  // Wrap before converting so large u cannot overflow int.
  int x0 =
      static_cast<int>(std::fmod(x_floor, static_cast<float>(image.width)));
  x0 += x0 < 0 ? image.width : 0;
  const int x1 = (x0 + 1) % image.width;
  const int y0 = static_cast<int>(y);
  const int y1 = std::min(y0 + 1, image.height - 1);
  const float ty = y - static_cast<float>(y0);
  const glm::vec3 top =
      glm::mix(Texel(image, x0, y0), Texel(image, x1, y0), tx);
  const glm::vec3 bottom =
      glm::mix(Texel(image, x0, y1), Texel(image, x1, y1), tx);
  return glm::mix(top, bottom, ty);
}

glm::vec3 EvaluateIrradiance(const IrradianceSh9& sh, glm::vec3 normal) {
  const std::array<float, 9> basis = ShPolynomials(normal);
  glm::vec3 irradiance(0.0f);
  for (size_t i = 0; i < basis.size(); ++i) {
    irradiance += sh.coefficients[i] * basis[i];
  }
  return irradiance;
}

IrradianceSh9 ComputeIrradianceSh9(const asset::ImageDataF& image,
                                   int max_width, float clamp_luminance) {
  IrradianceSh9 sh;
  if (image.width <= 0 || image.height <= 0) {
    return sh;
  }
  const asset::ImageDataF source =
      Downsample(image, max_width, clamp_luminance);
  // Accumulate in double: a 2k image sums millions of small terms.
  std::array<glm::dvec3, 9> sums{};
  double total_weight = 0.0;
  for (int y = 0; y < source.height; ++y) {
    const double weight = TexelSolidAngle(source.width, source.height, y);
    for (int x = 0; x < source.width; ++x) {
      const glm::vec3 direction =
          TexelDirection(source.width, source.height, x, y);
      const glm::dvec3 radiance = glm::dvec3(Texel(source, x, y)) * weight;
      const std::array<float, 9> basis = ShPolynomials(direction);
      for (size_t i = 0; i < basis.size(); ++i) {
        sums[i] += radiance * static_cast<double>(basis[i]);
      }
      total_weight += weight;
    }
  }
  // The discrete solid angles sum to slightly less than 4 pi.
  const double normalization = 4.0 * kPiD / total_weight;
  for (size_t i = 0; i < sums.size(); ++i) {
    // L_lm = K * sum; E_lm = A_l * L_lm; the shader multiplies by K again.
    const double factor =
        kShBasis[i] * kShBasis[i] * kCosineLobe[i] * normalization;
    sh.coefficients[i] = glm::vec3(sums[i] * factor);
  }
  return sh;
}

BrdfLut GenerateBrdfLut(int size, int sample_count) {
  BrdfLut lut{.size = std::max(size, 1)};
  const int samples = std::max(sample_count, 1);
  lut.pixels.resize(static_cast<size_t>(lut.size) *
                    static_cast<size_t>(lut.size) * 2);
  for (int y = 0; y < lut.size; ++y) {
    const float roughness =
        (static_cast<float>(y) + 0.5f) / static_cast<float>(lut.size);
    for (int x = 0; x < lut.size; ++x) {
      const float n_dot_v =
          (static_cast<float>(x) + 0.5f) / static_cast<float>(lut.size);
      // Scale and bias are fractions of the reflected energy (clamped like
      // env_brdf_lut.frag's output).
      const glm::vec2 value =
          glm::clamp(IntegrateBrdf(n_dot_v, roughness, samples), 0.0f, 1.0f);
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(lut.size) +
           static_cast<size_t>(x)) *
          2;
      lut.pixels[offset] = value.x;
      lut.pixels[offset + 1] = value.y;
    }
  }
  return lut;
}

glm::vec3 BrightestDirection(const asset::ImageDataF& image) {
  if (image.width <= 0 || image.height <= 0) {
    return {0.0f, 1.0f, 0.0f};
  }
  const asset::ImageDataF source = Downsample(image, kBrightestSearchWidth);
  float brightest = 0.0f;
  for (int y = 0; y < source.height; ++y) {
    for (int x = 0; x < source.width; ++x) {
      brightest = std::max(brightest, Luminance(Texel(source, x, y)));
    }
  }
  if (!(brightest > 0.0f)) {
    return {0.0f, 1.0f, 0.0f};
  }
  glm::vec3 sum(0.0f);
  for (int y = 0; y < source.height; ++y) {
    const float solid_angle = TexelSolidAngle(source.width, source.height, y);
    for (int x = 0; x < source.width; ++x) {
      const float luminance = Luminance(Texel(source, x, y));
      if (luminance >= brightest * kBrightestFraction) {
        sum += TexelDirection(source.width, source.height, x, y) * luminance *
               solid_angle;
      }
    }
  }
  const float length = glm::length(sum);
  return length > 0.0f ? sum / length : glm::vec3(0.0f, 1.0f, 0.0f);
}

SunEstimate EstimateSun(const asset::ImageDataF& equirect) {
  SunEstimate estimate;
  if (equirect.width <= 0 || equirect.height <= 0) {
    return estimate;
  }
  // The sky's typical luminance: the median of a small copy.
  const asset::ImageDataF small = Downsample(equirect, kSunSearchWidth);
  std::vector<float> luminances;
  luminances.reserve(small.pixels.size() / 3);
  for (int y = 0; y < small.height; ++y) {
    for (int x = 0; x < small.width; ++x) {
      luminances.push_back(Luminance(Texel(small, x, y)));
    }
  }
  const auto middle =
      luminances.begin() + static_cast<ptrdiff_t>(luminances.size() / 2);
  std::ranges::nth_element(luminances, middle);
  const float threshold =
      std::max(kMinSunLuminance, kSunToSkyLuminanceRatio * *middle);

  // Everything above the threshold is the sun: its excess radiance, summed
  // over solid angle, is the irradiance it delivers.
  glm::dvec3 excess_irradiance(0.0);
  glm::dvec3 weighted_direction(0.0);
  const float* texel = equirect.pixels.data();
  for (int y = 0; y < equirect.height; ++y) {
    const double solid_angle =
        TexelSolidAngle(equirect.width, equirect.height, y);
    for (int x = 0; x < equirect.width; ++x, texel += 3) {
      // Plain floats for the common case: most texels are not the sun.
      const float luminance =
          kLumaR * texel[0] + kLumaG * texel[1] + kLumaB * texel[2];
      if (!(luminance > threshold)) {
        continue;
      }
      const glm::vec3 radiance(texel[0], texel[1], texel[2]);
      const glm::vec3 excess = radiance * (1.0f - threshold / luminance);
      excess_irradiance += glm::dvec3(excess) * solid_angle;
      weighted_direction +=
          glm::dvec3(TexelDirection(equirect.width, equirect.height, x, y)) *
          (static_cast<double>(Luminance(excess)) * solid_angle);
    }
  }
  const double length = glm::length(weighted_direction);
  if (!(length > 0.0)) {
    return estimate;
  }
  // Renderer convention: color * intensity is the radiance a white
  // Lambertian surface facing the light reflects, E / pi.
  const glm::vec3 radiance = glm::vec3(excess_irradiance / kPiD);
  const float peak = std::max({radiance.r, radiance.g, radiance.b});
  estimate.found = peak > 0.0f;
  estimate.direction = -glm::vec3(weighted_direction / length);
  estimate.color = estimate.found ? radiance / peak : glm::vec3(1.0f);
  estimate.intensity = peak;
  estimate.clamp_luminance = threshold;
  return estimate;
}

asset::ImageDataF MakeDefaultEnvironment(int width, int height) {
  asset::ImageDataF image{.width = std::max(width, 1),
                          .height = std::max(height, 1)};
  image.pixels.resize(static_cast<size_t>(image.width) *
                      static_cast<size_t>(image.height) * 3);
  const glm::vec3 zenith{0.30f, 0.42f, 0.65f};
  const glm::vec3 horizon{0.95f, 0.98f, 1.02f};
  const glm::vec3 ground{0.17f, 0.15f, 0.13f};
  for (int y = 0; y < image.height; ++y) {
    // Only the elevation matters, so one direction per row is enough.
    const float up = TexelDirection(image.width, image.height, 0, y).y;
    const glm::vec3 sky =
        glm::mix(horizon, zenith, std::sqrt(std::max(up, 0.0f)));
    // Blend over a few degrees around the horizon.
    const float t = glm::smoothstep(-0.04f, 0.02f, up);
    const glm::vec3 color = glm::mix(ground, sky, t);
    for (int x = 0; x < image.width; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * static_cast<size_t>(image.width) +
           static_cast<size_t>(x)) *
          3;
      image.pixels[offset] = color.r;
      image.pixels[offset + 1] = color.g;
      image.pixels[offset + 2] = color.b;
    }
  }
  return image;
}

}  // namespace blue::lighting
