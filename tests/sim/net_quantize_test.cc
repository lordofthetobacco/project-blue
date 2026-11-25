// Quantisation of positions, velocities, angles and rotations
// (docs/netcode/protocol.md section 4).

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

#include <glm/gtc/quaternion.hpp>

#include "net/quantize.h"

namespace blue::net {
namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfinity = std::numeric_limits<float>::infinity();
constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

TEST_CASE("fixed point rounds half away from zero and clamps") {
  CHECK(QuantizePosition(0.0f) == 0);
  CHECK(QuantizePosition(1.0f) == 512);
  CHECK(QuantizePosition(-3.0f) == -1536);
  CHECK(QuantizePosition(0.001f) == 1);            // 0.512 rounds up
  CHECK(QuantizePosition(1.0f / 1024.0f) == 1);    // exactly 0.5
  CHECK(QuantizePosition(-1.0f / 1024.0f) == -1);  // exactly -0.5
  CHECK(QuantizePosition(1.0e9f) == kPositionMax);
  CHECK(QuantizePosition(-1.0e9f) == kPositionMin);
  CHECK(QuantizePosition(kInfinity) == kPositionMax);
  CHECK(QuantizePosition(-kInfinity) == kPositionMin);
  CHECK(QuantizePosition(kNaN) == 0);
  CHECK(QuantizeVelocity(-9.8125f) == -628);
  CHECK(QuantizeVelocity(500.0f) == kVelocityMax);
  CHECK(DequantizePosition(-1408) == -2.75f);
  CHECK(DequantizeVelocity(kVelocityMin) == -128.0f);
}

TEST_CASE("yaw wraps into one turn") {
  CHECK(QuantizeYaw(0.0f, kFineAngleBits) == 0);
  CHECK(QuantizeYaw(kTau, kFineAngleBits) == 0);
  CHECK(QuantizeYaw(-std::numbers::pi_v<float> / 2.0f, kFineAngleBits) ==
        3u << 18);
  CHECK(QuantizeYaw(kNaN, kFineAngleBits) == 0);
  CHECK(QuantizeYaw(kInfinity, kFineAngleBits) == 0);
  CHECK(QuantizeYaw(1.0f, kCoarseAngleBits) == 10430);
  CHECK(QuantizeYaw(2.0f, kCoarseAngleBits) == 20861);
  for (uint32_t q : {0u, 1u, (1u << 20) - 1}) {
    const float yaw = DequantizeYaw(q, kFineAngleBits);
    CHECK(yaw >= 0.0f);
    CHECK(yaw < kTau);
    CHECK(QuantizeYaw(yaw, kFineAngleBits) == q);
  }
}

TEST_CASE("pitch clamps to a quarter turn") {
  CHECK(QuantizePitch(10.0f, kFineAngleBits) == 1 << 18);
  CHECK(QuantizePitch(-10.0f, kFineAngleBits) == -(1 << 18));
  CHECK(QuantizePitch(kNaN, kFineAngleBits) == 0);
  CHECK(PitchLimit(kCoarseAngleBits) == 16384);
  // The golden vectors' canonical command angles.
  CHECK(DequantizeYaw(QuantizeYaw(1.5f, kFineAngleBits), kFineAngleBits) ==
        1.4999995f);
  CHECK(DequantizePitch(QuantizePitch(-0.25f, kFineAngleBits),
                        kFineAngleBits) == -0.25000292f);
}

TEST_CASE("smallest-three quaternions") {
  SUBCASE("the identity is exact") {
    const QuatQ q = QuantizeQuat(glm::quat::wxyz(1.0f, 0.0f, 0.0f, 0.0f));
    CHECK(q.largest == 3);
    CHECK(q.components[0] == 511);
    const glm::quat back = DequantizeQuat(q);
    CHECK(back.w == 1.0f);
    CHECK(back.x == 0.0f);
  }
  SUBCASE("degenerate input becomes the identity") {
    CHECK(QuantizeQuat(glm::quat::wxyz(0.0f, 0.0f, 0.0f, 0.0f)) == QuatQ{});
    CHECK(QuantizeQuat(glm::quat::wxyz(kNaN, 0.0f, 0.0f, 0.0f)) == QuatQ{});
  }
  SUBCASE("the sign is folded into the largest component") {
    const QuatQ q = QuantizeQuat(glm::quat::wxyz(-0.8f, 0.0f, -0.6f, 0.0f));
    CHECK(q.largest == 3);
    const glm::quat back = DequantizeQuat(q);
    CHECK(back.w > 0.0f);
    CHECK(back.y > 0.0f);
  }
  SUBCASE("ties pick the first index") {
    const QuatQ q = QuantizeQuat(glm::quat::wxyz(0.5f, 0.5f, 0.5f, 0.5f));
    CHECK(q.largest == 0);
  }
  SUBCASE("the golden vector rotation") {
    const glm::quat back =
        DequantizeQuat(QuantizeQuat(glm::quat::wxyz(0.8f, 0.0f, 0.6f, 0.0f)));
    CHECK(back.x == 0.0f);
    CHECK(back.y == 0.60055643f);
    CHECK(back.z == 0.0f);
    CHECK(back.w == 0.79958236f);
  }
  SUBCASE("components clamp to 1022") {
    QuatQ q;
    q.components = {5000, 0, 511};
    const glm::quat back = DequantizeQuat(q);
    CHECK(std::isfinite(back.w));
  }
}

}  // namespace
}  // namespace blue::net
