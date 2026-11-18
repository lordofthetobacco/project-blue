#include "material/material.h"

#include <algorithm>
#include <cmath>

namespace blue::material {
namespace {

// Returns `value` clamped to [min, max], or `fallback` if it is not finite.
float ClampFinite(float value, float min, float max, float fallback) {
  return std::isfinite(value) ? std::clamp(value, min, max) : fallback;
}

}  // namespace

MaterialDesc Sanitize(MaterialDesc desc) {
  const MaterialDesc defaults;
  for (int i = 0; i < 4; ++i) {
    desc.base_color[i] =
        ClampFinite(desc.base_color[i], 0.0f, 1.0f, defaults.base_color[i]);
  }
  desc.metallic = ClampFinite(desc.metallic, 0.0f, 1.0f, defaults.metallic);
  desc.roughness = ClampFinite(desc.roughness, 0.0f, 1.0f, defaults.roughness);
  for (int i = 0; i < 3; ++i) {
    // Emissive colours may exceed 1 (HDR); only finiteness and sign matter.
    desc.emissive[i] = ClampFinite(desc.emissive[i], 0.0f, kMaxEmissiveStrength,
                                   defaults.emissive[i]);
  }
  desc.emissive_strength =
      ClampFinite(desc.emissive_strength, 0.0f, kMaxEmissiveStrength,
                  defaults.emissive_strength);
  desc.normal_scale = ClampFinite(desc.normal_scale, 0.0f, kMaxNormalScale,
                                  defaults.normal_scale);
  desc.occlusion_strength = ClampFinite(desc.occlusion_strength, 0.0f, 1.0f,
                                        defaults.occlusion_strength);
  for (int i = 0; i < 2; ++i) {
    desc.uv_scale[i] = ClampFinite(desc.uv_scale[i], 1.0e-4f, kMaxUvScale,
                                   defaults.uv_scale[i]);
  }
  desc.alpha_cutoff =
      ClampFinite(desc.alpha_cutoff, 0.0f, 1.0f, defaults.alpha_cutoff);
  return desc;
}

bool SameAppearance(const MaterialDesc& a, const MaterialDesc& b) {
  MaterialDesc renamed = b;
  renamed.name = a.name;
  return a == renamed;
}

float RoughnessFromShininess(float shininess) {
  if (!std::isfinite(shininess) || shininess <= 0.0f) {
    return 1.0f;
  }
  return std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.05f, 1.0f);
}

}  // namespace blue::material
