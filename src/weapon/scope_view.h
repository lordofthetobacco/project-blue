#ifndef BLUE_WEAPON_SCOPE_VIEW_H_
#define BLUE_WEAPON_SCOPE_VIEW_H_

#include <cstdint>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include "weapon/scope.h"
#include "weapon/scope_optics.h"
#include "weapon/viewmodel.h"

// What the renderer needs this frame to show a scope's picture in its
// eyepiece: the scope's own camera, the glass, the eye's placement and
// whether a picture is worth rendering at all.

namespace blue::weapon {

// The most pixels per side of a scope picture (render::kMaxScopeResolution).
inline constexpr uint32_t kMaxScopePictureSize = 1024;
// The fewest: below this the glass is too small on screen to show detail.
inline constexpr uint32_t kMinScopePictureSize = 64;
// Below this peak eye-box brightness no picture is rendered (the glass is
// drawn dark).
inline constexpr float kMinScopeTransmission = 0.01f;
// m the drawn glass is lifted from the eyepiece centre towards the eye, so
// it never fights the model's own glass faces.
inline constexpr float kEyepieceLift = 0.0005f;

// The main camera, for sizing the picture.
struct ScopeScreen {
  float vertical_fov_radians = 1.2f;
  float height_pixels = 1080.0f;
};

struct ScopeViewState {
  // The scope's camera: world -> view (x along the scope's right, y up,
  // looking down the axis), its position (the objective, on the axis) and
  // tan of the half field of its square picture.
  glm::mat4 camera_view{1.0f};
  glm::vec3 camera_position{0.0f};
  float camera_tangent = 0.0f;
  // The unit disc -> the eyepiece glass in world space (lifted by
  // kEyepieceLift towards the eye).
  glm::mat4 lens{1.0f};
  EyePlacement eye;
  float exit_pupil_radius = 0.0f;  // m
  // PeakEyeBoxTransmission() for the eye: 0 when no light reaches it.
  float peak_transmission = 0.0f;
  // The glass's diameter on screen, pixels (0 when behind the eye).
  float glass_pixels = 0.0f;
  // Pixels per side of the picture: the glass's size on screen scaled to
  // the picture's wider field, rounded up to 32, within
  // [kMinScopePictureSize, kMaxScopePictureSize].
  uint32_t picture_size = kMinScopePictureSize;
  // Whether the picture is worth rendering: some light reaches the eye
  // (kMinScopeTransmission), the eye is behind the glass and the glass
  // covers a few pixels.
  bool render_picture = false;
};

// Returns the scope view of `spec` at `magnification` for the weapon posed
// by `viewmodel` (eyepiece_center(), lens_right/up(), optic_axis()) seen
// from the camera at `eye` with `screen`. Pure, no allocation.
[[nodiscard]] ScopeViewState MakeScopeViewState(const ScopeSpec& spec,
                                                const Viewmodel& viewmodel,
                                                glm::vec3 eye,
                                                float magnification,
                                                const ScopeScreen& screen);

// Returns the factor on the look sensitivity while looking through a scope
// of `magnification` at `aim_fraction` (0 hip .. 1 aimed): 1 at the hip,
// 1 / magnification fully aimed (so the picture in the glass moves about
// as fast on screen as the unaimed view does), linear between.
[[nodiscard]] float ScopeLookScale(float magnification, float aim_fraction);

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_SCOPE_VIEW_H_
