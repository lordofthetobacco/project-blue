#ifndef BLUE_SCENE_DEFAULT_LIGHTING_H_
#define BLUE_SCENE_DEFAULT_LIGHTING_H_

#include <glm/vec3.hpp>

// Lighting of the default (shooting range) scene, calibrated against its
// HDRI, samples/env/kloofendal_48d_partly_cloudy_puresky_2k.hdr.
//
// Units: light intensities follow render::PointLight's convention (color *
// intensity = radiance of a white Lambert surface facing the light), and the
// environment's diffuse light on a white surface is E / pi with E the SH9
// irradiance (lighting::EvaluateIrradiance). Measured on the HDRI with its
// sun clamped out (lighting::EstimateSun, ComputeIrradianceSh9 with the
// estimate's clamp_luminance), at environment intensity 1:
//   sky on a horizontal white surface, E_up / pi    ~ 0.49
//   the HDRI's sun: 48 deg elevation, intensity      ~ 1.41
// so the captured sun:sky ratio is only ~2:1 (partly cloudy, and the
// circumsolar glow stays in the sky). That gives flat, washed-out shadows; a
// clear-day look needs roughly 5-10:1. We keep the sky as captured and make
// the analytic sun 5x the sky on a horizontal surface:
//   sun = 5 * 0.49 / sin(48 deg) = 3.3
// A horizontal white surface in sun then gets 0.49 + 2.45 = 2.94, so an
// exposure of -1.5 EV (x 0.354) brings a sunlit 18% grey card to 0.187,
// i.e. mid grey on screen (every tonemapper keeps 0.18 at 0 EV).
// The panorama is rotated by -1.9 rad so that its sun stands over the
// shooter's right shoulder (behind and right of the firing line, which
// looks down -Z): targets are front-lit and their shadows fall down range.
// The sun's direction is the HDRI's, rotated the same way.

namespace blue::scene {

inline constexpr float kDefaultEnvironmentIntensity = 1.0f;
inline constexpr float kDefaultEnvironmentRotation = -1.9f;  // radians
inline constexpr float kDefaultExposureEv = -1.5f;

// glm::angleAxis(kDefaultEnvironmentRotation, +Y) * the EstimateSun direction
// (-0.3776, -0.7417, 0.5544).
inline constexpr glm::vec3 kDefaultSunDirection{-0.402539f, -0.741699f,
                                                -0.536512f};
// The estimate's colour (maximum component 1): a slightly warm white.
inline constexpr glm::vec3 kDefaultSunColor{0.99f, 1.0f, 0.91f};
inline constexpr float kDefaultSunToSkyRatio = 5.0f;  // horizontal surface
inline constexpr float kDefaultSunIntensity = 3.3f;

// The point light by the spawn, 3.5 m up. In full daylight it only needs to
// read as a lamp: 6 / 3.5^2 = 0.49 on the ground below, about as much as the
// whole sky adds there, a fifth of the sunlit level.
inline constexpr float kDefaultPointLightIntensity = 6.0f;

}  // namespace blue::scene

#endif  // BLUE_SCENE_DEFAULT_LIGHTING_H_
