#include "tests/render/image_compare.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

namespace blue::render_test {
namespace {

// A `width` x `height` image of one colour.
RgbaImage Solid(uint32_t width, uint32_t height, uint8_t r, uint8_t g,
                uint8_t b) {
  RgbaImage image{.width = width, .height = height};
  image.rgba.resize(size_t{width} * height * 4);
  for (size_t i = 0; i < image.rgba.size(); i += 4) {
    image.rgba[i] = r;
    image.rgba[i + 1] = g;
    image.rgba[i + 2] = b;
    image.rgba[i + 3] = 255;
  }
  return image;
}

TEST_CASE("Identical images have no difference and infinite PSNR") {
  const RgbaImage image = Solid(8, 4, 10, 20, 30);
  const CompareMetrics metrics = Compare(image, image, 0);
  CHECK(metrics.same_size);
  CHECK(metrics.mean_abs_diff == 0.0);
  CHECK(metrics.max_diff == 0);
  CHECK(metrics.outlier_fraction == 0.0);
  CHECK(std::isinf(metrics.psnr_db));
  CHECK(Matches(image, image, kExact));
}

TEST_CASE("Alpha is ignored") {
  const RgbaImage a = Solid(2, 2, 1, 2, 3);
  RgbaImage b = a;
  b.rgba[3] = 0;
  CHECK(Matches(a, b, kExact));
}

TEST_CASE("Different sizes never match") {
  const CompareMetrics metrics =
      Compare(Solid(2, 2, 0, 0, 0), Solid(2, 3, 0, 0, 0), 0);
  CHECK_FALSE(metrics.same_size);
  CHECK_FALSE(Passes(metrics, kSoftwareAdapter));
  CHECK_FALSE(Matches(RgbaImage{}, RgbaImage{}, kSoftwareAdapter));
}

TEST_CASE("One channel off by one: exact fails, exact-or-one passes") {
  const RgbaImage reference = Solid(10, 10, 100, 100, 100);
  RgbaImage actual = reference;
  actual.rgba[0] = 101;
  const CompareMetrics metrics = Compare(actual, reference, 0);
  CHECK(metrics.max_diff == 1);
  // 1 over 300 channel samples.
  CHECK(metrics.mean_abs_diff == doctest::Approx(1.0 / 300.0));
  CHECK(metrics.outlier_fraction == doctest::Approx(0.01));
  // MSE = 1/300 -> 10 log10(65025 * 300).
  CHECK(metrics.psnr_db == doctest::Approx(10.0 * std::log10(65025.0 * 300.0)));
  CHECK_FALSE(Matches(actual, reference, kExact));
  CHECK(Matches(actual, reference, kExactOrOne));
}

TEST_CASE("Outliers count pixels above the threshold") {
  const RgbaImage reference = Solid(10, 10, 50, 50, 50);
  RgbaImage actual = reference;
  // Two pixels: one 9 above (outlier for threshold 8), one 8 above (not).
  actual.rgba[0 * 4 + 1] = 59;
  actual.rgba[1 * 4 + 2] = 58;
  const CompareMetrics metrics = Compare(actual, reference, 8);
  CHECK(metrics.max_diff == 9);
  CHECK(metrics.outlier_fraction == doctest::Approx(0.01));
}

TEST_CASE("Cross-backend tolerance rejects a visibly different image") {
  const RgbaImage reference = Solid(16, 16, 128, 128, 128);
  CHECK(Matches(Solid(16, 16, 128, 128, 128), reference, kCrossBackend));
  // Mean 1/255 is above the 0.5 limit even with no outliers.
  CHECK_FALSE(Matches(Solid(16, 16, 129, 129, 129), reference, kCrossBackend));
  // But within the software adapter's 1.5.
  CHECK(Matches(Solid(16, 16, 129, 129, 129), reference, kSoftwareAdapter));
}

TEST_CASE("Amplified diff scales and clamps the difference") {
  const RgbaImage reference = Solid(1, 1, 10, 10, 10);
  const RgbaImage actual = Solid(1, 1, 12, 10, 200);
  const RgbaImage diff = AmplifiedDiff(actual, reference, 16);
  REQUIRE(diff.valid());
  CHECK(diff.rgba[0] == 32);
  CHECK(diff.rgba[1] == 0);
  CHECK(diff.rgba[2] == 255);
  CHECK(diff.rgba[3] == 255);
}

}  // namespace
}  // namespace blue::render_test
