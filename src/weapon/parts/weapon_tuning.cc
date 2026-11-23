#include "weapon/parts/weapon_tuning.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

#include "weapon/firearm.h"
#include "weapon/parts/weapon_stats.h"
#include "weapon/viewmodel.h"
#include "weapon/weapon_specs.h"

namespace blue::weapon::parts {
namespace {

// Handling never gets more than twice as quick as the reference.
constexpr float kMinHandlingScale = 0.5f;
constexpr float kPercent = 100.0f;

// Returns `value / reference`, or 1 for a reference that is not positive.
float Ratio(float value, float reference) {
  return reference > 0.0f ? value / reference : 1.0f;
}

}  // namespace

float ClampZero(const ZeroRange& range, float distance) {
  const float low = std::max(range.min, 1.0f);
  return std::clamp(distance, low, std::max(range.max, low));
}

float StepZero(const ZeroRange& range, float distance, int steps) {
  return ClampZero(range, ClampZero(range, distance) +
                              static_cast<float>(steps) * range.step);
}

float HandlingScale(float ergonomics, float reference) {
  return std::max(1.0f + (reference - ergonomics) / kPercent,
                  kMinHandlingScale);
}

BuildTuning TuneFromStats(const WeaponStats& stats,
                          std::string_view display_name) {
  const PartBuiltBase base = PartBuiltBaseFor(stats.weapon_kind);
  BuildTuning tuning{.spec = base.spec,
                     .viewmodel = base.viewmodel,
                     .aim_fov_scale = base.aim_fov_scale,
                     .accuracy_moa = stats.accuracy_moa};

  FirearmSpec& spec = tuning.spec;
  spec.display_name = display_name;
  spec.magazine_capacity = stats.magazine_capacity;
  if (stats.fire_rate > 0.0f) {
    spec.rounds_per_minute = stats.fire_rate;
  }
  spec.fire_modes =
      stats.fire_modes != 0 ? stats.fire_modes : FireModeBit(FireMode::kSemi);
  spec.fire_mode = stats.default_fire_mode;
  if (stats.muzzle_velocity > 0.0f) {
    spec.muzzle_velocity = stats.muzzle_velocity;
  }
  spec.kind = stats.weapon_kind;
  if (stats.weight > 0.0f) {
    spec.mass = stats.weight;
  }
  spec.functional = stats.functional;

  ViewmodelTuning& feel = tuning.viewmodel;
  const float vertical =
      Ratio(stats.recoil_vertical, base.reference_recoil_vertical);
  const float horizontal =
      Ratio(stats.recoil_horizontal, base.reference_recoil_horizontal);
  feel.look_kick_pitch *= vertical;
  feel.recoil_pitch_velocity *= vertical;
  feel.look_kick_yaw *= horizontal;

  const float handling =
      HandlingScale(stats.ergonomics, base.reference_ergonomics);
  feel.aim_time *= handling;
  feel.sway_gain *= handling;
  tuning.switch_time_scale = handling;

  feel.muzzle_velocity = spec.muzzle_velocity;
  if (stats.sighting_range > 0.0f) {
    const ZeroRange range{
        .min = std::min(kMinZeroDistance, stats.sighting_range),
        .max = stats.sighting_range,
        .step = kZeroStep};
    tuning.zero_range = range;
    feel.zero_distance = ClampZero(range, feel.zero_distance);
  }
  return tuning;
}

std::string StatsSummary(const WeaponStats& stats) {
  const std::string accuracy =
      stats.accuracy_moa > 0.0f ? std::format("{:.1f} MOA", stats.accuracy_moa)
                                : std::string("- MOA");
  const std::string sights = stats.sighting_range > 0.0f
                                 ? std::format("{:.0f} m", stats.sighting_range)
                                 : std::string("no sights");
  return std::format(
      "{:.2f} kg  ergo {:.0f}  {}  {}  recoil {:.0f}/{:.0f}  "
      "{:.0f} rpm",
      stats.weight, stats.ergonomics, accuracy, sights, stats.recoil_vertical,
      stats.recoil_horizontal, stats.fire_rate);
}

std::string MissingSummary(const WeaponStats& stats) {
  if (stats.missing_required.empty()) {
    return {};
  }
  std::string text = "NOT FUNCTIONAL: missing ";
  for (size_t i = 0; i < stats.missing_required.size(); ++i) {
    text += i == 0 ? "" : ", ";
    text += stats.missing_required[i];
  }
  return text;
}

}  // namespace blue::weapon::parts
