#include "core/det_math.h"

#include <doctest/doctest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <limits>
#include <numbers>

namespace blue::core {
namespace {

// The hash of the fixed input/output table below. It must be the same on
// MSVC (Windows) and GCC (Linux): that is the point of these functions.
constexpr uint64_t kExpectedTableHash = 0x0843e7265822f2b5ull;

constexpr float kInf = std::numeric_limits<float>::infinity();
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// Returns the error of `value` against the double `reference`, relative to
// max(|reference|, 1) so values near 0 are judged absolutely.
double Error(float value, double reference) {
  return std::fabs(static_cast<double>(value) - reference) /
         std::max(std::fabs(reference), 1.0);
}

// Returns `count` evenly spread values over [lo, hi] (exact float steps).
template <typename Check>
void Sweep(double lo, double hi, int count, Check check) {
  for (int i = 0; i <= count; ++i) {
    const double x =
        lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(count);
    check(static_cast<float>(x));
  }
}

TEST_CASE("Deterministic sin and cos are accurate to 2e-7 over +-1e4") {
  double worst_sin = 0.0;
  double worst_cos = 0.0;
  Sweep(-1.0e4, 1.0e4, 2000003, [&](float x) {
    const SinCos both = DetSinCos(x);
    CHECK(both.sin == DetSin(x));
    CHECK(both.cos == DetCos(x));
    const auto d = static_cast<double>(x);
    worst_sin = std::max(worst_sin, std::fabs(both.sin - std::sin(d)));
    worst_cos = std::max(worst_cos, std::fabs(both.cos - std::cos(d)));
  });
  MESSAGE(
      std::format("worst sin error {:.3g}, cos {:.3g}", worst_sin, worst_cos));
  CHECK(worst_sin <= 2.0e-7);
  CHECK(worst_cos <= 2.0e-7);
}

TEST_CASE("Deterministic tan, atan, atan2, asin and exp are accurate") {
  double worst = 0.0;
  Sweep(-1.5, 1.5, 300001, [&](float x) {
    worst = std::max(worst, Error(DetTan(x), std::tan(static_cast<double>(x))));
  });
  // Away from the poles, any period.
  Sweep(-1.0e3, 1.0e3, 200003, [&](float x) {
    const double t = std::tan(static_cast<double>(x));
    if (std::fabs(t) < 100.0) {
      worst = std::max(worst, Error(DetTan(x), t) / 2.0);
    }
  });
  Sweep(-1.0e3, 1.0e3, 200003, [&](float x) {
    worst =
        std::max(worst, Error(DetAtan(x), std::atan(static_cast<double>(x))));
  });
  Sweep(-1.0, 1.0, 200001, [&](float x) {
    worst =
        std::max(worst, Error(DetAsin(x), std::asin(static_cast<double>(x))));
  });
  for (int i = 0; i < 3600; ++i) {
    const double angle = static_cast<double>(i) * std::numbers::pi / 1800.0;
    for (const double radius : {1.0e-3, 1.0, 7.5, 1.0e4}) {
      const auto y = static_cast<float>(radius * std::sin(angle));
      const auto x = static_cast<float>(radius * std::cos(angle));
      worst = std::max(
          worst, Error(DetAtan2(y, x), std::atan2(static_cast<double>(y),
                                                  static_cast<double>(x))));
    }
  }
  Sweep(-80.0, 80.0, 160001, [&](float x) {
    const double e = std::exp(static_cast<double>(x));
    worst = std::max(worst, std::fabs(DetExp(x) - e) / e);
  });
  MESSAGE(std::format("worst relative error {:.3g}", worst));
  CHECK(worst <= 2.0e-7);
}

TEST_CASE("Deterministic functions are exact where it matters") {
  CHECK(DetSin(0.0f) == 0.0f);
  CHECK(std::signbit(DetSin(-0.0f)));
  CHECK(DetCos(0.0f) == 1.0f);
  CHECK(DetTan(0.0f) == 0.0f);
  CHECK(DetAtan(0.0f) == 0.0f);
  CHECK(DetAtan2(0.0f, 1.0f) == 0.0f);
  CHECK(DetAsin(0.0f) == 0.0f);
  CHECK(DetExp(0.0f) == 1.0f);
  CHECK(DetAsin(1.0f) == static_cast<float>(std::numbers::pi / 2.0));
  CHECK(DetAtan2(1.0f, 0.0f) == static_cast<float>(std::numbers::pi / 2.0));
  CHECK(DetAtan2(0.0f, -1.0f) == static_cast<float>(std::numbers::pi));
  CHECK(DetAtan2(-0.0f, -1.0f) == -static_cast<float>(std::numbers::pi));
  CHECK(std::signbit(DetAtan2(-0.0f, 1.0f)));
  CHECK(DetAtan(kInf) == static_cast<float>(std::numbers::pi / 2.0));
  CHECK(DetAtan2(kInf, kInf) == static_cast<float>(std::numbers::pi / 4.0));
  CHECK(DetExp(kInf) == kInf);
  CHECK(DetExp(-kInf) == 0.0f);
  CHECK(DetExp(1000.0f) == kInf);
  CHECK(DetExp(-1000.0f) == 0.0f);
  CHECK(std::isnan(DetSin(kInf)));
  CHECK(std::isnan(DetCos(kNaN)));
  CHECK(std::isnan(DetTan(-kInf)));
  CHECK(std::isnan(DetAtan(kNaN)));
  CHECK(std::isnan(DetAtan2(kNaN, 1.0f)));
  CHECK(std::isnan(DetAsin(1.5f)));
  CHECK(std::isnan(DetExp(kNaN)));
  // Unit length: sin^2 + cos^2 within float rounding.
  for (float x = -10.0f; x < 10.0f; x += 0.37f) {
    const SinCos both = DetSinCos(x);
    CHECK(both.sin * both.sin + both.cos * both.cos ==
          doctest::Approx(1.0f).epsilon(3e-7));
  }
}

TEST_CASE("Deterministic functions give the checked-in results everywhere") {
  uint64_t hash = 0xcbf29ce484222325ull;
  const auto add = [&hash](float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    for (int i = 0; i < 4; ++i) {
      hash ^= (bits >> (8 * i)) & 0xffu;
      hash *= 0x100000001b3ull;
    }
  };
  // 4096 inputs from an xorshift sequence spread over several ranges.
  uint32_t state = 0x9e3779b9u;
  for (int i = 0; i < 4096; ++i) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const float unit = static_cast<float>(state >> 8) / 16777216.0f;
    const float scale = (i % 4 == 0)   ? 4.0f
                        : (i % 4 == 1) ? 100.0f
                        : (i % 4 == 2) ? 1.0e4f
                                       : 1.0f;
    const float x = (2.0f * unit - 1.0f) * scale;
    add(DetSin(x));
    add(DetCos(x));
    add(DetTan(x));
    add(DetAtan(x));
    add(DetAtan2(x, 1.0f - unit));
    add(DetAsin(2.0f * unit - 1.0f));
    add(DetExp(x * 0.008f));
  }
  MESSAGE(std::format("det_math table hash: 0x{:016x}", hash));
  CHECK(hash == kExpectedTableHash);
}

}  // namespace
}  // namespace blue::core
