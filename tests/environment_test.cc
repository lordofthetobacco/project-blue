#include "lighting/environment.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <numbers>

#include <glm/geometric.hpp>

#include "asset/image_decoder.h"
#include "core/path.h"

namespace blue::lighting {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Returns a width x height equirect image where every texel is `fill(dir)`.
template <typename Fill>
asset::ImageDataF MakeImage(int width, int height, Fill fill) {
  asset::ImageDataF image{.width = width, .height = height};
  image.pixels.reserve(static_cast<size_t>(width * height * 3));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const glm::vec3 direction = EquirectUvToDirection(
          {(static_cast<float>(x) + 0.5f) / static_cast<float>(width),
           (static_cast<float>(y) + 0.5f) / static_cast<float>(height)});
      const glm::vec3 value = fill(direction);
      image.pixels.insert(image.pixels.end(), {value.r, value.g, value.b});
    }
  }
  return image;
}

bool Near(glm::vec3 a, glm::vec3 b, float epsilon) {
  return glm::length(a - b) <= epsilon;
}

}  // namespace

TEST_CASE("Equirect coordinates follow the documented convention") {
  CHECK(Near(glm::vec3(DirectionToEquirectUv({0, 0, -1}), 0), {0.5f, 0.5f, 0},
             1e-6f));
  CHECK(DirectionToEquirectUv({0, 1, 0}).y == doctest::Approx(0.0f));
  CHECK(DirectionToEquirectUv({0, -1, 0}).y == doctest::Approx(1.0f));
  CHECK(DirectionToEquirectUv({1, 0, 0}).x == doctest::Approx(0.75f));
  CHECK(DirectionToEquirectUv({-1, 0, 0}).x == doctest::Approx(0.25f));

  for (const glm::vec3 direction :
       {glm::vec3(0.3f, 0.5f, -0.8f), glm::vec3(-0.9f, -0.2f, 0.1f),
        glm::vec3(0.0f, 0.1f, 1.0f), glm::vec3(0.6f, -0.7f, 0.4f)}) {
    const glm::vec3 unit = glm::normalize(direction);
    CHECK(
        Near(EquirectUvToDirection(DirectionToEquirectUv(unit)), unit, 1e-5f));
  }
}

TEST_CASE("SampleEquirect interpolates and wraps u") {
  asset::ImageDataF image{.width = 2, .height = 1};
  image.pixels = {0, 0, 0, 1, 1, 1};
  // Texel centres at u = 0.25 and 0.75; u = 0 is halfway across the seam.
  CHECK(SampleEquirect(image, {0.25f, 0.5f}).x == doctest::Approx(0.0f));
  CHECK(SampleEquirect(image, {0.5f, 0.5f}).x == doctest::Approx(0.5f));
  CHECK(SampleEquirect(image, {0.0f, 0.5f}).x == doctest::Approx(0.5f));
  CHECK(SampleEquirect(image, {1.0f, 0.9f}).x == doctest::Approx(0.5f));
  CHECK(SampleEquirect(image, {-3.25f, 0.0f}).x == doctest::Approx(1.0f));
}

TEST_CASE("SH9 of a constant environment gives pi * L everywhere") {
  const glm::vec3 radiance{2.0f, 1.0f, 0.5f};
  const asset::ImageDataF image =
      MakeImage(128, 64, [&](glm::vec3) { return radiance; });

  const IrradianceSh9 sh = ComputeIrradianceSh9(image);

  for (const glm::vec3 normal :
       {glm::vec3(0, 1, 0), glm::vec3(0, -1, 0), glm::vec3(1, 0, 0),
        glm::normalize(glm::vec3(0.3f, -0.4f, 0.8f))}) {
    CHECK(Near(EvaluateIrradiance(sh, normal), kPi * radiance, 2e-3f));
  }
}

TEST_CASE("SH9 of a sky-only environment lights upward faces") {
  const asset::ImageDataF image = MakeImage(256, 128, [](glm::vec3 d) {
    return glm::vec3(d.y > 0.0f ? 1.0f : 0.0f);
  });

  const IrradianceSh9 sh = ComputeIrradianceSh9(image);

  // Exact: pi facing up, 0 facing down, pi / 2 sideways; SH9 is within a few
  // percent of pi.
  CHECK(EvaluateIrradiance(sh, {0, 1, 0}).x ==
        doctest::Approx(kPi).epsilon(0.1));
  CHECK(std::abs(EvaluateIrradiance(sh, {0, -1, 0}).x) < 0.1f * kPi);
  CHECK(EvaluateIrradiance(sh, {1, 0, 0}).x ==
        doctest::Approx(kPi / 2.0f).epsilon(0.02));
}

TEST_CASE("SH9 follows the direction of a bright region") {
  const asset::ImageDataF image = MakeImage(128, 64, [](glm::vec3 d) {
    return glm::vec3(d.x > 0.7f ? 10.0f : 0.1f);
  });
  const IrradianceSh9 sh = ComputeIrradianceSh9(image);
  CHECK(EvaluateIrradiance(sh, {1, 0, 0}).x >
        5.0f * EvaluateIrradiance(sh, {-1, 0, 0}).x);
  CHECK(EvaluateIrradiance(sh, {0, 0, 1}).x ==
        doctest::Approx(EvaluateIrradiance(sh, {0, 0, -1}).x).epsilon(0.02));
}

TEST_CASE("The BRDF LUT's smoothest rows are finite and in [0, 1]") {
  // 64 rows at the GPU's 1024 samples: rows 0..4 lie below roughness 0.072,
  // where GGX sampling divides x by x (see ImportanceSampleGgx in
  // shaders/environment.glsl).
  constexpr int kSize = 64;
  const BrdfLut lut = GenerateBrdfLut(kSize, 1024);
  REQUIRE(lut.pixels.size() == size_t{kSize} * kSize * 2);
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const size_t offset = (static_cast<size_t>(y) * kSize + x) * 2;
      const float scale = lut.pixels[offset];
      const float bias = lut.pixels[offset + 1];
      CAPTURE(x);
      CAPTURE(y);
      REQUIRE(std::isfinite(scale));
      REQUIRE(std::isfinite(bias));
      REQUIRE(scale >= 0.0f);
      REQUIRE(bias >= 0.0f);
      REQUIRE(scale <= 1.0f);
      REQUIRE(bias <= 1.0f);
    }
  }
  // A mirror seen head-on reflects everything through the F0 term.
  const size_t head_on = (kSize - 1) * 2;
  CHECK(lut.pixels[head_on] == doctest::Approx(1.0f).epsilon(0.02));
  CHECK(lut.pixels[head_on + 1] == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("The BRDF LUT matches split-sum reference values") {
  constexpr int kSize = 32;
  const BrdfLut lut = GenerateBrdfLut(kSize, 512);
  REQUIRE(lut.size == kSize);
  REQUIRE(lut.pixels.size() == size_t{kSize} * kSize * 2);
  const auto at = [&](int x, int y) {
    const size_t offset = (static_cast<size_t>(y) * kSize + x) * 2;
    return glm::vec2(lut.pixels[offset], lut.pixels[offset + 1]);
  };

  // Smooth surface seen head-on: all energy reflected, F0 term only.
  CHECK(at(kSize - 1, 0).x == doctest::Approx(1.0f).epsilon(0.02));
  CHECK(at(kSize - 1, 0).y == doctest::Approx(0.0f).epsilon(0.02));
  // A rough surface loses energy to single scattering: E = 0.45 at
  // NdotV = 0.5, roughness 1 (brute-force hemisphere integration).
  CHECK(at(kSize / 2, kSize - 1).x + at(kSize / 2, kSize - 1).y ==
        doctest::Approx(0.45f).epsilon(0.05));
  for (int y = 0; y < kSize; ++y) {
    for (int x = 0; x < kSize; ++x) {
      const glm::vec2 value = at(x, y);
      CHECK(value.x >= 0.0f);
      CHECK(value.y >= 0.0f);
      CHECK(value.x + value.y <= 1.01f);
    }
  }
  // Directional albedo falls as roughness rises (fixed NdotV = 0.5).
  CHECK(at(kSize / 2, kSize - 1).x + at(kSize / 2, kSize - 1).y <
        at(kSize / 2, 0).x + at(kSize / 2, 0).y);
}

TEST_CASE("BrightestDirection finds a hot spot") {
  const glm::vec3 sun = glm::normalize(glm::vec3(0.4f, 0.6f, -0.5f));
  const asset::ImageDataF image = MakeImage(256, 128, [&](glm::vec3 d) {
    return glm::vec3(glm::dot(d, sun) > 0.995f ? 500.0f : 1.0f);
  });
  CHECK(glm::dot(BrightestDirection(image), sun) > 0.99f);
  CHECK(BrightestDirection({}) == glm::vec3(0, 1, 0));
}

TEST_CASE("EstimateSun finds a small, very bright sun and its irradiance") {
  const glm::vec3 to_sun = glm::normalize(glm::vec3(-0.3f, 0.5f, 0.6f));
  constexpr float kSunRadiance = 20000.0f;
  constexpr float kCosRadius = 0.9994f;  // about 2 degrees
  const asset::ImageDataF image = MakeImage(1024, 512, [&](glm::vec3 d) {
    return glm::dot(d, to_sun) > kCosRadius ? glm::vec3(kSunRadiance)
                                            : glm::vec3(1.0f, 1.2f, 1.5f);
  });

  const SunEstimate sun = EstimateSun(image);

  REQUIRE(sun.found);
  CHECK(glm::dot(sun.direction, -to_sun) > 0.999f);
  CHECK(sun.color == glm::vec3(1.0f));
  // Removed irradiance over pi: (L - threshold) * solid angle / pi.
  const float solid_angle = 2.0f * kPi * (1.0f - kCosRadius);
  const float expected =
      (kSunRadiance - sun.clamp_luminance) * solid_angle / kPi;
  CHECK(sun.intensity == doctest::Approx(expected).epsilon(0.1));
  CHECK(sun.clamp_luminance >= 20.0f);
  CHECK(sun.clamp_luminance < 200.0f);

  // Clamped at the threshold, the sun hardly lights the SH any more.
  const IrradianceSh9 sky =
      ComputeIrradianceSh9(image, 256, sun.clamp_luminance);
  const IrradianceSh9 all = ComputeIrradianceSh9(image);
  const float lit_by_sky = EvaluateIrradiance(sky, to_sun).g;
  CHECK(lit_by_sky < 1.3f * kPi * 1.2f);
  CHECK(EvaluateIrradiance(all, to_sun).g > 2.0f * lit_by_sky);
}

TEST_CASE("EstimateSun finds nothing in an even sky") {
  const asset::ImageDataF image = MakeImage(256, 128, [](glm::vec3 d) {
    return glm::vec3(d.y > 0.0f ? 3.0f : 0.5f);
  });
  const SunEstimate sun = EstimateSun(image);
  CHECK_FALSE(sun.found);
  CHECK_FALSE(EstimateSun({}).found);
}

TEST_CASE("MakeDefaultEnvironment is a sky over a darker ground") {
  const asset::ImageDataF image = MakeDefaultEnvironment(64, 32);
  REQUIRE(image.pixels.size() == size_t{64} * 32 * 3);
  const glm::vec3 zenith = SampleEquirect(image, {0.5f, 0.0f});
  const glm::vec3 horizon = SampleEquirect(image, {0.5f, 0.47f});
  const glm::vec3 ground = SampleEquirect(image, {0.5f, 0.9f});
  CHECK(zenith.b > zenith.r);  // blue sky
  CHECK(horizon.g > zenith.g);
  CHECK(horizon.g == doctest::Approx(1.0f).epsilon(0.1));
  CHECK(ground.g == doctest::Approx(0.15f).epsilon(0.2));
}

TEST_CASE("IBL preprocessing of the sample HDRI and a 256 LUT is timed") {
  const auto hdr = asset::LoadHdrFile(core::JoinPath(
      BLUE_SAMPLES_DIR, "env/kloofendal_48d_partly_cloudy_puresky_2k.hdr"));
  REQUIRE(hdr.ok());

  auto start = std::chrono::steady_clock::now();
  const IrradianceSh9 sh = ComputeIrradianceSh9(hdr.value());
  const double sh_ms = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  start = std::chrono::steady_clock::now();
  const glm::vec3 sun = BrightestDirection(hdr.value());
  const SunEstimate estimate = EstimateSun(hdr.value());
  const double sun_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  start = std::chrono::steady_clock::now();
  const BrdfLut lut = GenerateBrdfLut(256);
  const double lut_ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - start)
                            .count();
  MESSAGE("sample sun: found ", estimate.found, ", direction (",
          estimate.direction.x, ", ", estimate.direction.y, ", ",
          estimate.direction.z, "), intensity ", estimate.intensity,
          ", threshold ", estimate.clamp_luminance);
  CHECK(estimate.found);
  CHECK(glm::dot(estimate.direction, -sun) > 0.99f);
  MESSAGE("SH9 of 2048 x 1024: ", sh_ms, " ms; sun search: ", sun_ms,
          " ms; 256^2 BRDF LUT (256 samples): ", lut_ms, " ms");

  // Daylight: the sky lights upward faces more than the ground lights
  // downward ones, and the sun is above the horizon.
  CHECK(EvaluateIrradiance(sh, {0, 1, 0}).y >
        EvaluateIrradiance(sh, {0, -1, 0}).y);
  CHECK(sun.y > 0.0f);
  CHECK(lut.pixels.size() == size_t{256} * 256 * 2);
}

}  // namespace blue::lighting
