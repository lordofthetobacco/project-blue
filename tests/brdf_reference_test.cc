#include "render/brdf_reference.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

namespace blue::render::brdf_reference {
namespace {

// Largest finite half float: what the RGBA16F HDR target can hold.
constexpr float kHalfMax = 65504.0f;
// The default scene's sun (scene/default_lighting.h) and a bright point light
// a few centimetres away.
constexpr std::array<float, 2> kIntensities = {3.3f, 1000.0f};
constexpr std::array<float, 11> kRoughness = {
    0.0f, 0.001f, 0.01f, 0.045f, 0.05f, 0.1f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
constexpr std::array<float, 2> kMetallic = {0.0f, 1.0f};
// Cosines to the normal, exact 0 and +-1 included; negative ones are below
// the surface (normal-mapped normals can face away from the camera).
constexpr std::array<float, 10> kCosines = {
    -1.0f, -0.5f, -1.0e-6f, 0.0f, 1.0e-6f, 1.0e-3f, 0.1f, 0.5f, 0.999f, 1.0f};
constexpr std::array<float, 3> kAzimuths = {0.0f, 1.5707964f, 3.1415927f};

bool Finite(const glm::vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool NonNegative(const glm::vec3& v) {
  return v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f;
}

// Unit direction at `cosine` to +Z and `azimuth` about it.
glm::vec3 Direction(float cosine, float azimuth) {
  const float sine = std::sqrt(std::max(1.0f - cosine * cosine, 0.0f));
  return {sine * std::cos(azimuth), sine * std::sin(azimuth), cosine};
}

// The i-th of `count` Hammersley points (environment.glsl).
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

float ExactDivide(float numerator, float denominator) {
  return numerator / denominator;
}

// A GPU-like division (x * rcp(y)) whose x / x comes out two ulp above 1 for
// x != 1, which Vulkan's 2.5 ulp OpFDiv precision allows; 1 / 1 stays exact.
// (One ulp would round back to 1 in the correctly rounded CPU sqrt; GPU sqrt
// is approximate as well.)
float GpuDivide(float numerator, float denominator) {
  return numerator == denominator && numerator != 1.0f
             ? std::nextafter(std::nextafter(1.0f, 2.0f), 2.0f)
             : numerator / denominator;
}

// sin(theta) as ImportanceSampleGgx computed it before the fix, kept to
// document the failure: no clamp before the square roots.
float UnclampedSinTheta(float xi_y, float alpha, Divide quotient) {
  const float cos_theta =
      std::sqrt(quotient(1.0f - xi_y, 1.0f + (alpha * alpha - 1.0f) * xi_y));
  return std::sqrt(1.0f - cos_theta * cos_theta);
}

TEST_CASE("Direct lighting is finite and non-negative for every legal input") {
  const glm::vec3 normal(0.0f, 0.0f, 1.0f);
  float peak = 0.0f;
  for (const float roughness : kRoughness) {
    for (const float metallic : kMetallic) {
      for (const float view_cos : kCosines) {
        for (const float light_cos : kCosines) {
          for (const float azimuth : kAzimuths) {
            const glm::vec3 view = Direction(view_cos, 0.0f);
            const Surface s =
                MakeSurface(normal, view, glm::vec3(1.0f), metallic, roughness);
            for (const float intensity : kIntensities) {
              // Every light direction of the grid, and the light exactly
              // opposite the view (no halfway vector).
              for (const glm::vec3& to_light :
                   {Direction(light_cos, azimuth), -view}) {
                const glm::vec3 color =
                    DirectLight(s, to_light, glm::vec3(intensity));
                CAPTURE(roughness);
                CAPTURE(metallic);
                CAPTURE(view_cos);
                CAPTURE(light_cos);
                CAPTURE(azimuth);
                REQUIRE(Finite(color));
                REQUIRE(NonNegative(color));
                const glm::vec3 written = SanitizeRadiance(color);
                REQUIRE(Finite(written));
                REQUIRE(written.x <= kMaxSceneRadiance);
                if (intensity == kIntensities[0]) {
                  peak = std::max(peak, color.x);
                }
              }
            }
          }
        }
      }
    }
  }
  // The sun's GGX peak on a near-mirror is far beyond what half floats hold
  // (+Inf in the HDR target without the clamp), yet finite in fp32.
  MESSAGE("peak sun radiance off a mirror before the clamp: ", peak);
  CHECK(peak > kHalfMax);
  CHECK(kMaxSceneRadiance * 6.0f < kHalfMax);  // Six bloom levels summed.
}

TEST_CASE("Sanitising maps NaN to 0 and infinities into range") {
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();
  const glm::vec3 color = SanitizeRadiance({nan, inf, -inf});
  CHECK(color.x == 0.0f);
  CHECK(color.y == kMaxSceneRadiance);
  CHECK(color.z == 0.0f);
  CHECK(SafeNormalize(glm::vec3(0.0f), {0, 1, 0}) == glm::vec3(0, 1, 0));
  CHECK(SafeNormalize(glm::vec3(nan), {0, 1, 0}) == glm::vec3(0, 1, 0));
  CHECK(SafeNormalize(glm::vec3(inf, 0, 0), {0, 1, 0}) == glm::vec3(0, 1, 0));
  CHECK(glm::length(SafeNormalize({3, 4, 0}, {0, 1, 0})) ==
        doctest::Approx(1.0f));
}

TEST_CASE("Specular occlusion stays in [0, 1] and follows ao on mirrors") {
  for (const float roughness : kRoughness) {
    for (int ao_step = -2; ao_step <= 10; ++ao_step) {
      // Below 0: an occlusion strength above 1 (mesh.frag now clamps it).
      const float ao = static_cast<float>(ao_step) / 10.0f;
      float previous = -1.0f;
      for (const float n_dot_v : {1.0e-4f, 0.01f, 0.1f, 0.5f, 0.9f, 1.0f}) {
        const float occlusion = SpecularOcclusion(n_dot_v, ao, roughness);
        CAPTURE(roughness);
        CAPTURE(ao);
        CAPTURE(n_dot_v);
        REQUIRE(std::isfinite(occlusion));
        REQUIRE(occlusion >= 0.0f);
        REQUIRE(occlusion <= 1.0f);
        CHECK(occlusion >= previous);  // Less occluded towards head-on.
        previous = occlusion;
      }
    }
  }
  // A mirror seen head-on keeps about the ambient occlusion: dark screen-space
  // AO darkens its reflection by the same amount.
  CHECK(SpecularOcclusion(1.0f, 0.3f, kMinPerceptualRoughness) ==
        doctest::Approx(0.383f).epsilon(0.02));
  CHECK(SpecularOcclusion(1.0f, 0.0f, kMinPerceptualRoughness) == 0.0f);
  CHECK(SpecularOcclusion(1.0f, 1.0f, kMinPerceptualRoughness) == 1.0f);
}

TEST_CASE(
    "Ambient weights stay finite for white metals and broken LUT texels") {
  for (const float roughness : kRoughness) {
    for (const float metallic : kMetallic) {
      // Base colours past 1 (an over-bright factor) included.
      for (const float base : {0.0f, 0.5f, 1.0f, 4.0f}) {
        const Surface s = MakeSurface({0, 0, 1}, {0, 0, 1}, glm::vec3(base),
                                      metallic, roughness);
        // Texels of EnvironmentBrdf: clamped to [0, 1] each, so their sum
        // reaches 2; (0, 0) is a white metal that reflects nothing.
        for (const glm::vec2 lut :
             {glm::vec2(0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(0.5f, 0.1f),
              glm::vec2(1.0f), glm::vec2(0.0f, 1.0f)}) {
          const glm::vec3 weights = AmbientWeights(s, lut);
          CAPTURE(roughness);
          CAPTURE(metallic);
          CAPTURE(base);
          CAPTURE(lut.x);
          CAPTURE(lut.y);
          REQUIRE(Finite(weights));
          REQUIRE(NonNegative(weights));
          CHECK(weights.x <= 2.0f);
          CHECK(weights.y <= 2.0e4f);  // 1 / 1e-4 at worst
        }
      }
    }
  }
}

TEST_CASE("GGX importance samples stay finite under inexact GPU division") {
  constexpr uint32_t kCount = 1024;  // env_brdf_lut.frag's sample count
  constexpr int kLutSize = 256;      // render::Environment::kBrdfLutSize
  int broken_rows_before = 0;
  int last_broken_row = -1;
  for (int row = 0; row < kLutSize; ++row) {
    // The LUT's perceptual roughness at this row's texel centre.
    const float roughness =
        (static_cast<float>(row) + 0.5f) / static_cast<float>(kLutSize);
    const float alpha = roughness * roughness;
    bool broken_before = false;
    for (uint32_t i = 0; i < kCount; ++i) {
      const glm::vec2 xi = Hammersley(i, kCount);
      for (const Divide divide : {&ExactDivide, &GpuDivide}) {
        const glm::vec3 h = ImportanceSampleGgx(xi, alpha, divide);
        CAPTURE(row);
        CAPTURE(i);
        REQUIRE(Finite(h));
        REQUIRE(glm::length(h) == doctest::Approx(1.0f).epsilon(1.0e-5));
        REQUIRE(h.z >= 0.0f);
      }
      // With exact division the old formula was fine on the CPU.
      REQUIRE(std::isfinite(UnclampedSinTheta(xi.y, alpha, &ExactDivide)));
      broken_before |= std::isnan(UnclampedSinTheta(xi.y, alpha, &GpuDivide));
    }
    if (broken_before) {
      ++broken_rows_before;
      last_broken_row = row;
    }
  }
  // Evidence for the black patches: one NaN sample makes a whole LUT row NaN,
  // and every row up to the minimum roughness (0.045, row 11) the mesh
  // shader reads at the centre of a smooth sphere was one.
  MESSAGE("LUT rows NaN before the fix (GPU-like division): ",
          broken_rows_before, ", up to row ", last_broken_row, " (roughness ",
          (static_cast<float>(last_broken_row) + 0.5f) / kLutSize, ")");
  CHECK(last_broken_row >= 12);

  // The degenerate corner: alpha 0 and xi.y = 1 is 0 / 0.
  CHECK(Finite(ImportanceSampleGgx({0.5f, 1.0f}, 0.0f, &ExactDivide)));
}

}  // namespace
}  // namespace blue::render::brdf_reference
