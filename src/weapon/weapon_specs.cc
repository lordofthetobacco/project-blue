#include "weapon/weapon_specs.h"

#include <optional>

namespace blue::weapon {

FirearmSpec M16Spec() { return FirearmSpec{}; }

ViewmodelTuning M16ViewmodelTuning() {
  ViewmodelTuning tuning;
  tuning.muzzle_velocity = M16Spec().muzzle_velocity;
  return tuning;
}

FirearmSpec Glock17Spec() {
  return {
      .display_name = "Glock 17",
      .magazine_capacity = 17,
      .rounds_per_minute = 1100.0f,
      .tactical_reload_seconds = 1.7f,
      .empty_reload_seconds = 2.1f,
      .muzzle_velocity = 375.0f,
      .bullet_mass = 0.008f,
      .bullet_drag = 1.7e-3f,
      .spare_magazines = 3,
      .fire_mode = FireMode::kSemi,
      .fire_modes = FireModeBit(FireMode::kSemi),
      .kind = WeaponKind::kPistol,
      .mass = 0.905f,
  };
}

ViewmodelTuning Glock17ViewmodelTuning() {
  ViewmodelTuning tuning;
  // Both hands on the grip, forearms angled in: low centre, well ahead.
  tuning.hip = {{0.06f, -0.11f, -0.36f}, {0.0f, 0.03f, 0.02f}};
  // Muzzle down at the chest ("sul"-like), close in.
  tuning.sprint = {{0.04f, -0.24f, -0.20f}, {-0.8f, 0.35f, 0.25f}};
  // Pulled in and canted to bring the magazine well towards the eye.
  tuning.reload = {{0.03f, -0.15f, -0.28f}, {0.35f, 0.3f, 0.55f}};
  tuning.holster = {{0.05f, -0.40f, -0.20f}, {-1.0f, 0.2f, 0.3f}};
  tuning.eye_relief = 0.45f;
  tuning.aim_time = 0.15f;
  tuning.zero_distance = 25.0f;
  tuning.muzzle_velocity = Glock17Spec().muzzle_velocity;
  tuning.recoil_back_velocity = 0.0f;
  tuning.recoil_pitch_velocity = 0.0f;
  tuning.look_kick_pitch = 0.008f;
  tuning.look_kick_yaw = 0.004f;
  tuning.aim_recoil_scale = 0.8f;
  return tuning;
}

FirearmSpec SprA3GSpec() {
  constexpr float kSecondsPerMinute = 60.0f;
  return {
      .display_name = "FN SPR A3G",
      .magazine_capacity = 5,
      .rounds_per_minute = kSecondsPerMinute / kSprA3GBoltCycleSeconds,
      .tactical_reload_seconds = 2.6f,
      .empty_reload_seconds = 3.1f,
      .muzzle_velocity = 790.0f,
      .bullet_mass = 0.01134f,
      .bullet_drag = 7.3e-4f,
      .spare_magazines = 4,
      .fire_mode = FireMode::kSemi,
      .fire_modes = FireModeBit(FireMode::kSemi),
      .kind = WeaponKind::kRifle,
      .mass = 6.5f,
  };
}

ViewmodelTuning SprA3GViewmodelTuning() {
  ViewmodelTuning tuning;
  // A little lower and further out than the M16: the scope sits high.
  tuning.hip = {{0.11f, -0.15f, -0.24f}, {0.0f, 0.02f, 0.05f}};
  tuning.eye_relief = 0.09f;
  tuning.aim_time = 0.3f;
  tuning.zero_distance = 100.0f;
  tuning.muzzle_velocity = SprA3GSpec().muzzle_velocity;
  tuning.bullet_drag = SprA3GSpec().bullet_drag;
  tuning.sway_gain = 0.8f;
  tuning.aim_motion_scale = 0.0f;
  tuning.recoil_back_velocity = 1.6f;
  tuning.recoil_pitch_velocity = 2.4f;
  tuning.look_kick_pitch = 0.012f;
  tuning.look_kick_yaw = 0.003f;
  return tuning;
}

ScopeSpec SprA3GScope() {
  return {.min_magnification = 3.5f,
          .max_magnification = 10.0f,
          .step = 0.5f,
          .start_magnification = 3.5f,
          .zoom_time = 0.12f,
          .min_zero = 100.0f,
          .max_zero = 300.0f,
          .zero_step = 50.0f,
          .start_zero = 100.0f,
          .optics = {.eyepiece_radius = 0.0163f,
                     .objective_radius = 0.020f,
                     .objective_distance = 0.326f,
                     .eye_relief = 0.096f,
                     .apparent_field = 0.18f,
                     .distortion = 0.035f,
                     .chromatic_aberration = 0.004f,
                     .vignetting = 0.35f,
                     .eye_pupil_radius = 0.003f,
                     .pupil_shadow_depth = 0.008f},
          .reticle = SimpleCrossReticle()};
}

PartBuiltBase PartBuiltBaseFor(WeaponKind kind) {
  if (kind == WeaponKind::kPistol) {
    // The stock preset's sums, in the order weapon_stats.cc forms them.
    constexpr float kBarrelRecoilPercent = -2.9f;
    constexpr float kRecoilScale = 1.0f + kBarrelRecoilPercent / 100.0f;
    return {.spec = Glock17Spec(),
            .viewmodel = Glock17ViewmodelTuning(),
            .aim_fov_scale = kPistolAimFovScale,
            .reference_ergonomics = 91.0f,
            .reference_recoil_vertical = 311.0f * kRecoilScale,
            .reference_recoil_horizontal = 260.0f * kRecoilScale};
  }
  return {.spec = M16Spec(),
          .viewmodel = M16ViewmodelTuning(),
          .aim_fov_scale = std::nullopt,
          .reference_ergonomics = 50.0f,
          .reference_recoil_vertical = 175.0f,
          .reference_recoil_horizontal = 163.0f};
}

}  // namespace blue::weapon
