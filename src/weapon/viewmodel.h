#ifndef BLUE_WEAPON_VIEWMODEL_H_
#define BLUE_WEAPON_VIEWMODEL_H_

#include <cstdint>

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include "camera/look.h"
#include "weapon/weapon_geometry.h"

namespace blue::weapon {

// A pose of the weapon relative to the eye, in camera space (x right, y up,
// -z forward, metres): where the sight line's eye point sits and how the
// weapon is turned about it from the aimed orientation (sight axis along -z,
// upright).
struct WeaponPose {
  glm::vec3 sight_offset{0.0f};      // m
  glm::vec3 rotation_radians{0.0f};  // pitch (up), yaw (left), roll (cw)
};

// Feel of the viewmodel. Rates are 1/s of exponential approach; angles are
// radians; distances metres.
struct ViewmodelTuning {
  // Hip: low and right of the eye, bore roughly parallel to the view.
  WeaponPose hip{{0.11f, -0.13f, -0.22f}, {0.0f, 0.02f, 0.05f}};
  // Sprint: lowered, pointed down-left and canted.
  WeaponPose sprint{{0.07f, -0.24f, -0.12f}, {-0.6f, 0.55f, 0.35f}};
  // Reload: tilted to show the magazine well.
  WeaponPose reload{{0.06f, -0.17f, -0.22f}, {0.25f, 0.2f, 0.6f}};
  // Holster: dropped below the view and tilted, muzzle down; blended in as
  // ViewmodelInput::raise_fraction falls to 0.
  WeaponPose holster{{0.08f, -0.40f, -0.16f}, {-0.9f, 0.3f, 0.5f}};
  // m from the eye to the sight line's eye point when aimed. Rifle: cheek
  // on the stock, which puts the butt about 0.1 m behind the eye.
  float eye_relief = 0.24f;
  float aim_time = 0.2f;        // s for most (~95 %) of the hip <-> aim blend
  float pose_rate = 8.0f;       // sprint and reload blend
  float zero_distance = 50.0f;  // m along the sight line
  float muzzle_velocity = 948.0f;  // m/s, for the zeroing angle
  float gravity = 9.81f;           // m/s^2, for the zeroing angle
  // 1/m, the bullet's quadratic drag (FirearmSpec::bullet_drag) for the
  // zeroing angle; 0 zeroes for a drag-free bullet.
  float bullet_drag = 0.0f;
  // Sway: the weapon lags behind turning by `sway_gain` times the angle turned
  // in a frame (clamped to `max_sway`) and springs back.
  float sway_gain = 0.6f;
  float max_sway = 0.08f;
  float sway_frequency = 9.0f;  // rad/s of the critically damped spring
  // Bob, at `bob_reference_speed` (scales linearly up to 1.5 times).
  float bob_frequency = 1.8f;               // Hz, steps per second
  glm::vec2 bob_amplitude{0.008f, 0.010f};  // m, lateral / vertical
  float bob_reference_speed = 4.0f;         // m/s
  float aim_motion_scale = 0.2f;  // sway and bob multiplier when fully aimed
  // Recoil per shot: weapon kick in camera space and camera kick.
  float recoil_back_velocity = 1.2f;   // m/s backwards per shot
  float recoil_pitch_velocity = 1.6f;  // rad/s muzzle rise per shot
  float recoil_frequency = 18.0f;      // rad/s of the recovery spring
  float look_kick_pitch = 0.0045f;     // rad camera rise per shot
  float look_kick_yaw = 0.0025f;       // rad, max horizontal spread per shot
  float aim_recoil_scale = 0.7f;       // recoil multiplier when fully aimed
  uint32_t seed = 0x5eedu;             // horizontal spread sequence
  // Inspect pose (the weapon inspect screen): the weapon's centre floats
  // `inspect_distance_per_radius` times its bounding radius ahead of the eye
  // (at least `inspect_min_distance`), moved by `inspect_offset` times that
  // distance (x right, y up) so the screen's panels have room, side-on with
  // the muzzle to the right, turned by the player's drag.
  float inspect_time = 0.35f;  // s for most (~95 %) of the blend
  float inspect_distance_per_radius = 2.8f;
  float inspect_min_distance = 0.3f;       // m
  glm::vec2 inspect_offset{-0.22f, 0.0f};  // share of the distance
  float inspect_max_pitch = 1.3f;          // rad, either way
};

// What the viewmodel needs from one frame.
struct ViewmodelInput {
  glm::vec3 eye_position{0.0f};  // world
  camera::LookAngles look;
  float roll = 0.0f;      // radians, as player::PlayerView::roll
  bool aim_held = false;  // ignored while sprinting or reloading
  bool sprinting = false;
  bool on_ground = true;
  glm::vec3 velocity{0.0f};  // world, m/s
  // Radians turned this frame as (yaw, pitch), signed like LookAngles (yaw
  // grows turning left, pitch looking up).
  glm::vec2 look_delta{0.0f};
  int shots = 0;  // fired this frame (FirearmEvents::shots)
  // Firearm::reload_fraction(): the reload tilt peaks half way through.
  float reload_fraction = 0.0f;
  bool reloading = false;
  // Loadout::raise_fraction(): 1 ready in the hands, 0 lowered out of view
  // (the holster pose). No aiming below 1.
  float raise_fraction = 1.0f;
  // The inspect screen is open: blend to the inspect pose (no sway, bob or
  // recoil in it) and turn the weapon by `inspect_turn`, (yaw, pitch)
  // radians this frame (+ yaw: its near side to the right, + pitch: up).
  bool inspecting = false;
  glm::vec2 inspect_turn{0.0f};
  float dt = 0.0f;  // seconds
};

// How far the inspect pose is blended in and how the player turned the
// weapon in it.
struct InspectState {
  float blend = 0.0f;      // 0 normal pose .. 1 inspect pose, unsmoothed
  glm::vec2 angles{0.0f};  // (yaw, pitch) radians, pitch clamped

  friend bool operator==(const InspectState&, const InspectState&) = default;
};

// Places a first-person weapon (rifle or pistol) relative to the player's
// eye. The weapon is rigidly attached to the eye frame (the same conventions
// as player::PlayerView::View(), roll included) and offset by a blend of hip,
// aimed, sprint, reload and holster poses plus sway, movement bob and recoil
// springs.
//
// Fully aimed with no motion, the sight line lies on the eye's forward axis
// with its eye point `eye_relief` ahead of the eye. The bore direction used
// for bullets is the sight axis pitched up by the zeroing angle
// (weapon/zeroing.h), so a bullet from the muzzle, under gravity and the
// tuning's drag, crosses the line of sight at the zero distance (the sight's
// adjustment is not modelled visually).
//
// Pure math on its inputs: deterministic, frame-rate independent, no
// allocation. Copyable. Not thread-safe.
class Viewmodel {
 public:
  explicit Viewmodel(const WeaponGeometry& geometry,
                     const ViewmodelTuning& tuning = {});
  Viewmodel(const Viewmodel&) = default;
  Viewmodel& operator=(const Viewmodel&) = default;

  void Update(const ViewmodelInput& input);

  // Clears the aim, sprint, reload and inspect blends, sway, bob, recoil
  // springs and pending look kick, for a weapon coming back into the hands.
  // The matrices keep their last values until the next Update.
  void Reset();

  // Sets the weapon's bounds in model space (centre and bounding-sphere
  // radius, model units) for the inspect pose. Until called, the centre is
  // half way between the grip and the muzzle and the radius half their
  // distance.
  void SetInspectBounds(glm::vec3 center, float radius);
  // The inspect blend and turn, e.g. to carry them over to the viewmodel of
  // the same weapon with other parts. The angles return to 0 once the blend
  // has gone back to 0.
  [[nodiscard]] const InspectState& inspect_state() const { return inspect_; }
  void set_inspect_state(const InspectState& state);
  // 0 in the normal pose .. 1 in the inspect pose, smoothed.
  [[nodiscard]] float inspect_fraction() const;

  // Weapon model space -> world, scale included.
  [[nodiscard]] glm::mat4 model_matrix() const { return model_; }
  // World position of the bore exit.
  [[nodiscard]] glm::vec3 muzzle_position() const { return muzzle_position_; }
  // Unit world direction of the zeroed bore as posed: fire bullets along it.
  [[nodiscard]] glm::vec3 muzzle_direction() const { return muzzle_direction_; }
  // World position of the pistol grip (for hands or effects).
  [[nodiscard]] glm::vec3 grip_position() const { return grip_position_; }
  // 0 at the hip .. 1 fully aimed, smoothed.
  [[nodiscard]] float aim_fraction() const { return aim_; }
  // Angle (radians) between the sight axis and the zeroed bore.
  [[nodiscard]] float zero_angle() const { return zero_angle_; }
  // m along the line of sight where the bullet crosses it.
  [[nodiscard]] float zero_distance() const { return tuning_.zero_distance; }
  // Re-zeroes the sight at `distance` m (a scope's elevation turret): the
  // bore direction changes from the next Update on.
  void SetZeroDistance(float distance);

  // The optic's lens as posed, for drawing the collimated dot: world centre,
  // radius in metres, unit right/up vectors of the lens plane and the unit
  // sight axis (downrange, the lens normal). The dot lies along the axis.
  // Without an optic (lens radius 0) the lens frame still follows the sight
  // line; draw no dot.
  [[nodiscard]] glm::vec3 lens_center() const { return lens_center_; }
  [[nodiscard]] float lens_radius() const {
    return geometry_.optic_lens_radius * geometry_.scale;
  }
  [[nodiscard]] glm::vec3 lens_right() const { return lens_right_; }
  [[nodiscard]] glm::vec3 lens_up() const { return lens_up_; }
  [[nodiscard]] glm::vec3 optic_axis() const { return optic_axis_; }
  // Returns the transform of the square [-1, 1]^2 at z = 0 onto the square
  // around the lens disc: x -> lens_right * radius, y -> lens_up * radius,
  // z -> optic_axis, origin -> lens_center. Clip to the unit disc for the
  // lens itself.
  [[nodiscard]] glm::mat4 lens_matrix() const;
  // World centre of a scope's eyepiece glass as posed (WeaponGeometry::
  // eyepiece); its plane is spanned by lens_right() and lens_up(), its
  // normal is optic_axis().
  [[nodiscard]] glm::vec3 eyepiece_center() const { return eyepiece_center_; }

  // Returns the camera recoil accumulated since the last call as (pitch up,
  // yaw left) radians, and clears it.
  [[nodiscard]] glm::vec2 TakeLookKick();

  [[nodiscard]] const ViewmodelTuning& tuning() const { return tuning_; }
  [[nodiscard]] const WeaponGeometry& geometry() const { return geometry_; }

 private:
  // Returns the next value in [-1, 1] of the spread sequence.
  float NextSpread();

  WeaponGeometry geometry_;
  ViewmodelTuning tuning_;
  // Model rotation that puts the sight axis along -z with the weapon upright.
  glm::quat aligned_{1.0f, 0.0f, 0.0f, 0.0f};
  // Unit sight axis and up in model space.
  glm::vec3 sight_forward_{0.0f, 0.0f, 1.0f};
  glm::vec3 sight_up_{0.0f, 1.0f, 0.0f};
  glm::vec3 bore_direction_model_{0.0f, 0.0f, 1.0f};  // unit, zeroed
  float zero_angle_ = 0.0f;

  float aim_ = 0.0f;
  float sprint_ = 0.0f;
  float reload_ = 0.0f;
  glm::vec2 sway_{0.0f};                // rad (yaw, pitch) offset
  glm::vec2 sway_velocity_{0.0f};       // rad/s
  float bob_phase_ = 0.0f;              // rad
  float bob_weight_ = 0.0f;             // [0, 1]
  float recoil_back_ = 0.0f;            // m
  float recoil_back_velocity_ = 0.0f;   // m/s
  float recoil_pitch_ = 0.0f;           // rad
  float recoil_pitch_velocity_ = 0.0f;  // rad/s
  glm::vec2 look_kick_{0.0f};
  InspectState inspect_;
  glm::vec3 inspect_center_{0.0f};  // model units
  float inspect_radius_ = 0.0f;     // model units
  uint32_t rng_state_ = 1;          // xorshift32 state, never 0

  glm::mat4 model_{1.0f};
  glm::vec3 muzzle_position_{0.0f};
  glm::vec3 muzzle_direction_{0.0f, 0.0f, -1.0f};
  glm::vec3 grip_position_{0.0f};
  glm::vec3 lens_center_{0.0f};
  glm::vec3 eyepiece_center_{0.0f};
  glm::vec3 lens_right_{1.0f, 0.0f, 0.0f};
  glm::vec3 lens_up_{0.0f, 1.0f, 0.0f};
  glm::vec3 optic_axis_{0.0f, 0.0f, -1.0f};
};

}  // namespace blue::weapon

#endif  // BLUE_WEAPON_VIEWMODEL_H_
