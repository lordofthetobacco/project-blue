#include "tests/render/image_compare.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

namespace blue::render_test {

CompareMetrics Compare(const RgbaImage& actual, const RgbaImage& reference,
                       uint32_t outlier_threshold) {
  CompareMetrics metrics;
  if (!actual.valid() || !reference.valid() ||
      actual.width != reference.width || actual.height != reference.height) {
    return metrics;
  }
  metrics.same_size = true;

  uint64_t sum_abs = 0;
  uint64_t sum_squared = 0;
  uint64_t outliers = 0;
  const size_t pixel_count = static_cast<size_t>(actual.width) * actual.height;
  for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
    uint32_t pixel_max = 0;
    for (size_t channel = 0; channel < 3; ++channel) {
      const size_t i = pixel * 4 + channel;
      const auto diff = static_cast<uint32_t>(
          std::abs(int{actual.rgba[i]} - int{reference.rgba[i]}));
      sum_abs += diff;
      sum_squared += uint64_t{diff} * diff;
      pixel_max = std::max(pixel_max, diff);
    }
    metrics.max_diff = std::max(metrics.max_diff, pixel_max);
    if (pixel_max > outlier_threshold) {
      ++outliers;
    }
  }

  const auto samples = static_cast<double>(pixel_count * 3);
  metrics.mean_abs_diff = static_cast<double>(sum_abs) / samples;
  metrics.outlier_fraction =
      static_cast<double>(outliers) / static_cast<double>(pixel_count);
  if (sum_squared > 0) {
    const double mse = static_cast<double>(sum_squared) / samples;
    metrics.psnr_db = 10.0 * std::log10(255.0 * 255.0 / mse);
  }
  return metrics;
}

bool Passes(const CompareMetrics& metrics, const Tolerance& tolerance) {
  return metrics.same_size && metrics.max_diff <= tolerance.max_diff &&
         metrics.mean_abs_diff <= tolerance.max_mean_abs_diff &&
         metrics.outlier_fraction <= tolerance.max_outlier_fraction &&
         metrics.psnr_db >= tolerance.min_psnr_db;
}

bool Matches(const RgbaImage& actual, const RgbaImage& reference,
             const Tolerance& tolerance) {
  return Passes(Compare(actual, reference, tolerance.outlier_threshold),
                tolerance);
}

std::string Describe(const CompareMetrics& metrics) {
  if (!metrics.same_size) {
    return "sizes differ (or an image is empty)";
  }
  return std::format(
      "mean {:.4f}/255, max {}/255, outliers {:.4f}%, PSNR {:.2f} dB",
      metrics.mean_abs_diff, metrics.max_diff,
      metrics.outlier_fraction * 100.0, metrics.psnr_db);
}

RgbaImage AmplifiedDiff(const RgbaImage& actual, const RgbaImage& reference,
                        uint32_t gain) {
  RgbaImage diff{.width = actual.width, .height = actual.height};
  diff.rgba.resize(actual.rgba.size());
  for (size_t i = 0; i < diff.rgba.size(); i += 4) {
    for (size_t channel = 0; channel < 3; ++channel) {
      const auto delta = static_cast<uint32_t>(std::abs(
          int{actual.rgba[i + channel]} - int{reference.rgba[i + channel]}));
      diff.rgba[i + channel] =
          static_cast<uint8_t>(std::min<uint32_t>(delta * gain, 255));
    }
    diff.rgba[i + 3] = 255;
  }
  return diff;
}

}  // namespace blue::render_test
