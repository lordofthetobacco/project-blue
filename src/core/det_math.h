#ifndef BLUE_CORE_DET_MATH_H_
#define BLUE_CORE_DET_MATH_H_

// Deterministic elementary functions: the same float result, bit for bit, on
// every platform and compiler the project supports. The C library's sinf,
// cosf, atanf, expf, ... are only accurate to an ulp or so, and MSVC and
// glibc round differently (sinf(0.1875f) differs in the last bit), which
// breaks a simulation that the dedicated server (Linux) and the client's
// prediction (Windows) must run identically.
//
// These evaluate in double precision with only IEEE-754 +, -, *, / (exact
// operations every conforming platform rounds the same way, given no FMA
// contraction: /fp:precise, -ffp-contract=off, see
// cmake/ProjectOptions.cmake), explicit argument reduction and fixed
// polynomials (fdlibm's), then round once to float. Results are within
// about half a float ulp of the true value: sin and cos to 2e-7 absolute
// over [-1e4, 1e4] (accuracy degrades, determinism does not, beyond 1e5).
//
// Use them on every simulation path. Rendering-only code may keep <cmath>.
// Non-finite inputs give NaN where the C library would (and the limits it
// gives for infinities in atan, atan2 and exp).

namespace blue::core {

// A sine and cosine of the same angle.
struct SinCos {
  float sin = 0.0f;
  float cos = 1.0f;
};

[[nodiscard]] float DetSin(float radians);
[[nodiscard]] float DetCos(float radians);
// Returns both, sharing the argument reduction.
[[nodiscard]] SinCos DetSinCos(float radians);
[[nodiscard]] float DetTan(float radians);
// Returns the arc tangent in [-pi/2, pi/2].
[[nodiscard]] float DetAtan(float x);
// Returns the angle of (x, y) in [-pi, pi], with std::atan2's conventions for
// zeros and infinities.
[[nodiscard]] float DetAtan2(float y, float x);
// Returns the arc sine in [-pi/2, pi/2]; NaN outside [-1, 1].
[[nodiscard]] float DetAsin(float x);
// Returns e^x (0 below about -104, infinity above about 89).
[[nodiscard]] float DetExp(float x);

}  // namespace blue::core

#endif  // BLUE_CORE_DET_MATH_H_
