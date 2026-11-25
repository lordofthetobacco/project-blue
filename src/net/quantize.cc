#include "net/quantize.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include <glm/gtc/quaternion.hpp>

namespace blue::net {
namespace {

// Binary64 constants of section 4 (Rust's core::f64::consts).
constexpr double kTau = 6.283185307179586;
constexpr double kSqrt2 = 1.4142135623730951;
constexpr double kFrac1Sqrt2 = 0.7071067811865476;
constexpr double kMinQuatNorm = 1e-6;

double Turn(int bits) { return static_cast<double>(uint32_t{1} << bits); }

double UnitsPerRadian(int bits) { return Turn(bits) / kTau; }

double RadiansPerUnit(int bits) { return kTau / Turn(bits); }

}  // namespace

int32_t QuantizeFixed(float x, double scale, int32_t min, int32_t max) {
  if (std::isnan(x)) {
    return 0;
  }
  const double q = std::round(static_cast<double>(x) * scale);
  if (q <= static_cast<double>(min)) {
    return min;
  }
  if (q >= static_cast<double>(max)) {
    return max;
  }
  return static_cast<int32_t>(q);
}

float DequantizeFixed(int32_t q, double scale) {
  return static_cast<float>(static_cast<double>(q) / scale);
}

int32_t QuantizePosition(float x) {
  return QuantizeFixed(x, kPositionScale, kPositionMin, kPositionMax);
}

float DequantizePosition(int32_t q) {
  return DequantizeFixed(q, kPositionScale);
}

int32_t QuantizeVelocity(float x) {
  return QuantizeFixed(x, kVelocityScale, kVelocityMin, kVelocityMax);
}

float DequantizeVelocity(int32_t q) {
  return DequantizeFixed(q, kVelocityScale);
}

uint32_t QuantizeYaw(float yaw, int bits) {
  if (!std::isfinite(yaw)) {
    return 0;
  }
  const double turn = Turn(bits);
  double q = std::fmod(
      std::round(static_cast<double>(yaw) * UnitsPerRadian(bits)), turn);
  if (q < 0.0) {
    q += turn;
  }
  // q is an integer in [0, turn) and turn <= 2^20: the cast is exact.
  return static_cast<uint32_t>(q);
}

float DequantizeYaw(uint32_t q, int bits) {
  return static_cast<float>(static_cast<double>(q) * RadiansPerUnit(bits));
}

int32_t PitchLimit(int bits) { return int32_t{1} << (bits - 2); }

int32_t QuantizePitch(float pitch, int bits) {
  const int32_t limit = PitchLimit(bits);
  return QuantizeFixed(pitch, UnitsPerRadian(bits), -limit, limit);
}

float DequantizePitch(int32_t q, int bits) {
  return static_cast<float>(static_cast<double>(q) * RadiansPerUnit(bits));
}

QuatQ QuantizeQuat(const glm::quat& rotation) {
  // Index order 0..3 is x, y, z, w (the wire order), independent of glm's
  // storage layout.
  std::array<double, 4> v = {
      static_cast<double>(rotation.x), static_cast<double>(rotation.y),
      static_cast<double>(rotation.z), static_cast<double>(rotation.w)};
  const double norm =
      std::sqrt(((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * v[3]);
  if (!std::isfinite(norm) || norm < kMinQuatNorm) {
    v = {0.0, 0.0, 0.0, 1.0};
  } else {
    for (double& c : v) {
      c = c / norm;
    }
  }
  int largest = 0;
  for (int i = 1; i < 4; ++i) {
    if (std::fabs(v[i]) > std::fabs(v[largest])) {
      largest = i;
    }
  }
  if (v[largest] < 0.0) {
    for (double& c : v) {
      c = -c;
    }
  }
  const double scale = static_cast<double>(kQuatComponentScale);
  QuatQ out;
  out.largest = static_cast<uint8_t>(largest);
  int slot = 0;
  for (int i = 0; i < 4; ++i) {
    if (i == largest) {
      continue;
    }
    const double scaled =
        std::clamp(std::round((v[i] * kSqrt2) * scale), -scale, scale);
    out.components[slot] = static_cast<uint32_t>(scaled + scale);
    ++slot;
  }
  return out;
}

glm::quat DequantizeQuat(const QuatQ& q) {
  const double scale = static_cast<double>(kQuatComponentScale);
  const int largest = std::min<int>(q.largest, 3);
  std::array<double, 4> v = {0.0, 0.0, 0.0, 0.0};
  double sum = 0.0;
  int slot = 0;
  for (int i = 0; i < 4; ++i) {
    if (i == largest) {
      continue;
    }
    const double raw =
        static_cast<double>(std::min(q.components[slot], kQuatComponentMax));
    const double c = ((raw - scale) / scale) * kFrac1Sqrt2;
    v[i] = c;
    sum = sum + c * c;
    ++slot;
  }
  v[largest] = std::sqrt(std::max(0.0, 1.0 - sum));
  return glm::quat::wxyz(static_cast<float>(v[3]), static_cast<float>(v[0]),
                         static_cast<float>(v[1]), static_cast<float>(v[2]));
}

}  // namespace blue::net
