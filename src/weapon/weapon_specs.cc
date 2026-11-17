#include "weapon/weapon_specs.h"

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
      .select_fire = false,
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

}  // namespace blue::weapon
