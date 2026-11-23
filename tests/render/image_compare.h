#ifndef BLUE_TESTS_RENDER_IMAGE_COMPARE_H_
#define BLUE_TESTS_RENDER_IMAGE_COMPARE_H_

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Comparison of rendered images (8-bit sRGB RGBA) against references for the
// headless render tests (tests/render, design §5.3). Pure C++, no GPU.

namespace blue::render_test {

// An 8-bit RGBA image, rows top to bottom.
struct RgbaImage {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4 bytes

  [[nodiscard]] bool valid() const {
    return width > 0 && height > 0 &&
           rgba.size() == static_cast<size_t>(width) * height * 4;
  }
};

// How two images differ, over the RGB channels (alpha is ignored: the
// output's alpha carries no colour).
struct CompareMetrics {
  bool same_size = false;
  // Mean absolute channel difference, in 1/255 units.
  double mean_abs_diff = 0.0;
  // Largest channel difference, in 1/255 units.
  uint32_t max_diff = 0;
  // Fraction [0, 1] of pixels with any channel differing by more than
  // Tolerance::outlier_threshold.
  double outlier_fraction = 0.0;
  // Peak signal-to-noise ratio in dB; infinity for identical images.
  double psnr_db = std::numeric_limits<double>::infinity();
};

// When a comparison passes; every condition must hold.
struct Tolerance {
  const char* name = "";
  uint32_t max_diff = 255;            // largest channel difference allowed
  double max_mean_abs_diff = 255.0;   // in 1/255 units
  uint32_t outlier_threshold = 255;   // a pixel is an outlier above this
  double max_outlier_fraction = 1.0;  // [0, 1]
  double min_psnr_db = 0.0;
};

// Design §5.3. Exact: refactors that must not change pixels.
inline constexpr Tolerance kExact{.name = "exact", .max_diff = 0};
// Exact, with one LSB allowed for drivers that are not bit-reproducible.
inline constexpr Tolerance kExactOrOne{.name = "exact-or-one", .max_diff = 1};
// Same backend after a shader compiler change (S1).
inline constexpr Tolerance kCompilerChange{.name = "compiler-change",
                                           .max_mean_abs_diff = 0.25,
                                           .outlier_threshold = 4,
                                           .max_outlier_fraction = 0.0005};
// Two backends on the same GPU.
inline constexpr Tolerance kCrossBackend{.name = "cross-backend",
                                         .max_mean_abs_diff = 0.5,
                                         .outlier_threshold = 8,
                                         .max_outlier_fraction = 0.001,
                                         .min_psnr_db = 45.0};
// A software adapter (WARP, lavapipe) against the hardware goldens.
inline constexpr Tolerance kSoftwareAdapter{.name = "software-adapter",
                                            .max_mean_abs_diff = 1.5,
                                            .outlier_threshold = 8,
                                            .max_outlier_fraction = 0.01,
                                            .min_psnr_db = 38.0};

// Compares `actual` with `reference`; `outlier_threshold` defines
// CompareMetrics::outlier_fraction. Images of different sizes (or invalid
// ones) give same_size = false and nothing else.
[[nodiscard]] CompareMetrics Compare(const RgbaImage& actual,
                                     const RgbaImage& reference,
                                     uint32_t outlier_threshold);

// Returns whether `metrics` (computed with tolerance.outlier_threshold)
// pass `tolerance`.
[[nodiscard]] bool Passes(const CompareMetrics& metrics,
                          const Tolerance& tolerance);

// Compares with tolerance.outlier_threshold and checks the result.
[[nodiscard]] bool Matches(const RgbaImage& actual, const RgbaImage& reference,
                           const Tolerance& tolerance);

// Returns a one-line description of `metrics`, for test output.
[[nodiscard]] std::string Describe(const CompareMetrics& metrics);

// Returns the per-channel absolute difference times `gain` (clamped), alpha
// 255: small differences become visible. Both images must have the same
// size.
[[nodiscard]] RgbaImage AmplifiedDiff(const RgbaImage& actual,
                                      const RgbaImage& reference,
                                      uint32_t gain);

}  // namespace blue::render_test

#endif  // BLUE_TESTS_RENDER_IMAGE_COMPARE_H_
