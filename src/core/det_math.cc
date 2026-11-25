#include "core/det_math.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

// The polynomials and constants are fdlibm's (Sun Microsystems, freely
// redistributable: "Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice is preserved."),
// evaluated in double without their low-order correction terms, which only
// matter for a double result. Only +, -, *, /, sqrt, floor, fabs and
// classification are used: no library transcendental.

namespace blue::core {
namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kPi = 3.14159265358979311600e+00;
constexpr double kPiOver2 = 1.57079632679489655800e+00;
constexpr double kPiOver4 = 7.85398163397448278999e-01;

// --- sin / cos (s_sin.c, k_sin.c, k_cos.c, e_rem_pio2.c) ---

constexpr double kTwoOverPi = 6.36619772367581382433e-01;
// pi/2 split so that k * kPiOver2Hi is exact for |k| < 2^20.
constexpr double kPiOver2Hi = 1.57079632673412561417e+00;  // first 33 bits
constexpr double kPiOver2Lo = 6.07710050650619224932e-11;  // pi/2 - Hi

constexpr double kS1 = -1.66666666666666324348e-01;
constexpr double kS2 = 8.33333333332248946124e-03;
constexpr double kS3 = -1.98412698298579493134e-04;
constexpr double kS4 = 2.75573137070700676789e-06;
constexpr double kS5 = -2.50507602534068634195e-08;
constexpr double kS6 = 1.58969099521155010221e-10;

constexpr double kC1 = 4.16666666666666019037e-02;
constexpr double kC2 = -1.38888888888741095749e-03;
constexpr double kC3 = 2.48015872894767294178e-05;
constexpr double kC4 = -2.75573143513906633035e-07;
constexpr double kC5 = 2.08757232129817482790e-09;
constexpr double kC6 = -1.13596475577881948265e-11;

// sin(r) for |r| <= pi/4.
double KernelSin(double r) {
  if (r == 0.0) {
    return r;  // Keeps the sign of -0.
  }
  const double z = r * r;
  return r +
         r * z *
             (kS1 + z * (kS2 + z * (kS3 + z * (kS4 + z * (kS5 + z * kS6)))));
}

// cos(r) for |r| <= pi/4.
double KernelCos(double r) {
  const double z = r * r;
  return 1.0 - 0.5 * z +
         z * z *
             (kC1 + z * (kC2 + z * (kC3 + z * (kC4 + z * (kC5 + z * kC6)))));
}

// The angle reduced to [-pi/4, pi/4] and its quadrant (0..3).
struct Reduced {
  double r = 0.0;
  int quadrant = 0;
};

Reduced Reduce(double x) {
  if (std::fabs(x) <= kPiOver4) {
    return {.r = x, .quadrant = 0};
  }
  const double k = std::floor(x * kTwoOverPi + 0.5);
  const double r = (x - k * kPiOver2Hi) - k * kPiOver2Lo;
  // k is an integer-valued double; its low two bits pick the quadrant.
  const double quarter = k - 4.0 * std::floor(k * 0.25);
  return {.r = r, .quadrant = static_cast<int>(quarter)};
}

SinCos SinCosOf(float radians) {
  if (!std::isfinite(radians)) {
    const auto nan = static_cast<float>(kNaN);
    return {.sin = nan, .cos = nan};
  }
  const Reduced reduced = Reduce(static_cast<double>(radians));
  const double s = KernelSin(reduced.r);
  const double c = KernelCos(reduced.r);
  double sin = s;
  double cos = c;
  switch (reduced.quadrant) {
    case 1:
      sin = c;
      cos = -s;
      break;
    case 2:
      sin = -s;
      cos = -c;
      break;
    case 3:
      sin = -c;
      cos = s;
      break;
    default:
      break;
  }
  return {.sin = static_cast<float>(sin), .cos = static_cast<float>(cos)};
}

// --- atan (s_atan.c) ---

constexpr double kAtanHi[] = {
    4.63647609000806093515e-01, 7.85398163397448278999e-01,
    9.82793723247329054082e-01, 1.57079632679489655800e+00};
constexpr double kAtanLo[] = {
    2.26987774529616870924e-17, 3.06161699786838301793e-17,
    1.39033110312309984516e-17, 6.12323399573676603587e-17};
constexpr double kAt[] = {
    3.33333333333329318027e-01, -1.99999999998764832476e-01,
    1.42857142725034663711e-01, -1.11111104054623557880e-01,
    9.09088713343650656196e-02, -7.69187620504482999495e-02,
    6.66107313738753120669e-02, -5.83357013379057348645e-02,
    4.97687799461593236017e-02, -3.65315727442169155270e-02,
    1.62858201153657823623e-02};

double AtanOf(double x) {
  if (std::isnan(x)) {
    return x;
  }
  const double ax = std::fabs(x);
  if (ax >= 7.3786976294838206464e+19) {  // 2^66: atan is pi/2 in double.
    return x > 0.0 ? kPiOver2 : -kPiOver2;
  }
  int id = -1;
  double t = x;
  if (ax < 0.4375) {
    if (ax < 7.450580596923828125e-09) {  // 2^-27
      return x;
    }
  } else {
    if (ax < 1.1875) {
      if (ax < 0.6875) {
        id = 0;
        t = (2.0 * ax - 1.0) / (2.0 + ax);
      } else {
        id = 1;
        t = (ax - 1.0) / (ax + 1.0);
      }
    } else if (ax < 2.4375) {
      id = 2;
      t = (ax - 1.5) / (1.0 + 1.5 * ax);
    } else {
      id = 3;
      t = -1.0 / ax;
    }
  }
  const double z = t * t;
  const double w = z * z;
  const double s1 =
      z *
      (kAt[0] +
       w * (kAt[2] + w * (kAt[4] + w * (kAt[6] + w * (kAt[8] + w * kAt[10])))));
  const double s2 =
      w * (kAt[1] + w * (kAt[3] + w * (kAt[5] + w * (kAt[7] + w * kAt[9]))));
  if (id < 0) {
    return t - t * (s1 + s2);
  }
  const double result = kAtanHi[id] - ((t * (s1 + s2) - kAtanLo[id]) - t);
  return x < 0.0 ? -result : result;
}

double Atan2Of(double y, double x) {
  if (std::isnan(x) || std::isnan(y)) {
    return kNaN;
  }
  const bool y_negative = std::signbit(y);
  const double sign = y_negative ? -1.0 : 1.0;
  if (y == 0.0) {
    // atan2(+-0, +x or +0) = +-0; atan2(+-0, -x or -0) = +-pi.
    return std::signbit(x) ? sign * kPi : y;
  }
  if (x == 0.0) {
    return sign * kPiOver2;
  }
  if (std::isinf(x)) {
    if (std::isinf(y)) {
      return sign * (x > 0.0 ? kPiOver4 : 3.0 * kPiOver4);
    }
    return x > 0.0 ? sign * 0.0 : sign * kPi;
  }
  if (std::isinf(y)) {
    return sign * kPiOver2;
  }
  const double z = AtanOf(std::fabs(y / x));
  if (x > 0.0) {
    return sign * z;
  }
  return sign * (kPi - z);
}

// --- exp (e_exp.c) ---

constexpr double kLn2Hi = 6.93147180369123816490e-01;
constexpr double kLn2Lo = 1.90821492927058770002e-10;
constexpr double kInvLn2 = 1.44269504088896338700e+00;
constexpr double kP1 = 1.66666666666666019037e-01;
constexpr double kP2 = -2.77777777770155933842e-03;
constexpr double kP3 = 6.61375632143793436117e-05;
constexpr double kP4 = -1.65339022054652515390e-06;
constexpr double kP5 = 4.13813679705723846039e-08;

// Returns 2^k for a k in the normal double range.
double PowerOfTwo(int k) {
  return std::bit_cast<double>(static_cast<uint64_t>(1023 + k) << 52);
}

double ExpOf(double x) {
  if (std::isnan(x)) {
    return x;
  }
  // Beyond these the float result is infinity or 0 anyway.
  if (x > 100.0) {
    return std::numeric_limits<double>::infinity();
  }
  if (x < -110.0) {
    return 0.0;
  }
  const double k = std::floor(x * kInvLn2 + 0.5);
  const double hi = x - k * kLn2Hi;
  const double lo = k * kLn2Lo;
  const double r = hi - lo;
  const double t = r * r;
  const double c = r - t * (kP1 + t * (kP2 + t * (kP3 + t * (kP4 + t * kP5))));
  const double y = 1.0 - ((lo - (r * c) / (2.0 - c)) - hi);
  return y * PowerOfTwo(static_cast<int>(k));
}

}  // namespace

float DetSin(float radians) { return SinCosOf(radians).sin; }

float DetCos(float radians) { return SinCosOf(radians).cos; }

SinCos DetSinCos(float radians) { return SinCosOf(radians); }

float DetTan(float radians) {
  if (!std::isfinite(radians)) {
    return static_cast<float>(kNaN);
  }
  const Reduced reduced = Reduce(static_cast<double>(radians));
  const double s = KernelSin(reduced.r);
  const double c = KernelCos(reduced.r);
  // tan has period pi: odd quadrants are -cot.
  const double tan = reduced.quadrant % 2 == 0 ? s / c : -c / s;
  return static_cast<float>(tan);
}

float DetAtan(float x) {
  return static_cast<float>(AtanOf(static_cast<double>(x)));
}

float DetAtan2(float y, float x) {
  return static_cast<float>(
      Atan2Of(static_cast<double>(y), static_cast<double>(x)));
}

float DetAsin(float x) {
  const auto d = static_cast<double>(x);
  if (!(std::fabs(d) <= 1.0)) {
    return static_cast<float>(kNaN);
  }
  return static_cast<float>(Atan2Of(d, std::sqrt((1.0 - d) * (1.0 + d))));
}

float DetExp(float x) {
  return static_cast<float>(ExpOf(static_cast<double>(x)));
}

}  // namespace blue::core
