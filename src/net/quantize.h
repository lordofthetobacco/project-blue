#ifndef BLUE_NET_QUANTIZE_H_
#define BLUE_NET_QUANTIZE_H_

#include <array>
#include <cstdint>

#include <glm/gtc/quaternion.hpp>

namespace blue::net {

// Quantisation of floats to wire integers and back, section 4 of
// docs/netcode/protocol.md. All arithmetic is binary64 in exactly the order
// the spec writes it, without contraction (the build compiles every
// first-party target with /fp:precise or -ffp-contract=off), and round() is
// std::round (half away from zero), so these functions produce the same
// integers as the Rust server bit for bit. Quantisers are total: NaN gives 0
// and values outside the range clamp.

// Positions: 1/512 m per unit, +-16384 m.
inline constexpr double kPositionScale = 512.0;
inline constexpr int32_t kPositionMin = -(1 << 23);
inline constexpr int32_t kPositionMax = (1 << 23) - 1;

// Linear and angular velocities: 1/64 m/s (rad/s) per unit, +-128.
inline constexpr double kVelocityScale = 64.0;
inline constexpr int32_t kVelocityMin = -(1 << 13);
inline constexpr int32_t kVelocityMax = (1 << 13) - 1;

// Angle resolutions: a full turn is 1 << bits units.
inline constexpr int kFineAngleBits = 20;    // UserCommand
inline constexpr int kCoarseAngleBits = 16;  // remote players

// Smallest-three quaternion components: 10 bits, 0..=1022.
inline constexpr uint32_t kQuatComponentScale = 511;
inline constexpr uint32_t kQuatComponentMax = 2 * kQuatComponentScale;

// Returns round(x * scale) clamped to min..=max; NaN gives 0.
int32_t QuantizeFixed(float x, double scale, int32_t min, int32_t max);
// Returns q / scale rounded to binary32.
float DequantizeFixed(int32_t q, double scale);

int32_t QuantizePosition(float x);
float DequantizePosition(int32_t q);
int32_t QuantizeVelocity(float x);
float DequantizeVelocity(int32_t q);

// Returns `yaw` in 0..(1 << bits) units of a full turn, wrapping; NaN and
// infinities give 0.
uint32_t QuantizeYaw(float yaw, int bits);
// Returns radians in [0, 2*pi).
float DequantizeYaw(uint32_t q, int bits);

// Returns the largest quantised pitch magnitude: a quarter turn.
int32_t PitchLimit(int bits);
// Returns `pitch` in units of 1 << bits per turn, clamped to a quarter turn
// either way; NaN gives 0.
int32_t QuantizePitch(float pitch, int bits);
float DequantizePitch(int32_t q, int bits);

// A smallest-three quaternion: the index (0..3 for x, y, z, w) of the
// dropped largest component and the other three in index order, each in
// 0..=kQuatComponentMax.
struct QuatQ {
  uint8_t largest = 3;
  std::array<uint32_t, 3> components = {
      kQuatComponentScale, kQuatComponentScale, kQuatComponentScale};

  friend bool operator==(const QuatQ&, const QuatQ&) = default;
};

// Quantises a rotation. The input is normalised first; a zero-length or
// non-finite input becomes the identity.
QuatQ QuantizeQuat(const glm::quat& rotation);
// Inverse of QuantizeQuat. Out-of-range fields are clamped.
glm::quat DequantizeQuat(const QuatQ& q);

}  // namespace blue::net

#endif  // BLUE_NET_QUANTIZE_H_
