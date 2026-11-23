#ifndef BLUE_RENDER_METAL_SAMPLERS_H_
#define BLUE_RENDER_METAL_SAMPLERS_H_

#include "render/metal/metal_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "render/metal/metal_context.h"
#include "render/temporal_math.h"

namespace blue::render::metal {

// Anisotropy of the material sampler (Metal's maxAnisotropy; every Metal 4
// device supports up to 16).
inline constexpr uint32_t kMaterialAnisotropy = 8;

// The samplers shared by the whole renderer, all usable in an argument table
// (BindSampler()). Neither copyable nor movable.
// Thread-safe to read.
class Samplers {
 public:
  explicit Samplers(const MetalContext& context);

  Samplers(const Samplers&) = delete;
  Samplers& operator=(const Samplers&) = delete;

  // Material textures: trilinear, repeating, anisotropic.
  [[nodiscard]] id<MTLSamplerState> material() const { return material_[0]; }
  // Same, with the mip level shifted by the supported bias nearest to
  // `lod_bias` (TextureLodBiasStep() in render/temporal_math.h): negative
  // biases sample sharper mips, for a scene rendered below the resolution
  // it is shown at (TextureLodBias()).
  [[nodiscard]] id<MTLSamplerState> material(float lod_bias) const {
    return material_[TextureLodBiasStep(lod_bias)];
  }
  // Full-screen inputs and render targets: bilinear (and linear between
  // mips), clamped to the edge.
  [[nodiscard]] id<MTLSamplerState> linear_clamp() const {
    return linear_clamp_;
  }
  // Exact texel fetches by UV, clamped to the edge.
  [[nodiscard]] id<MTLSamplerState> nearest_clamp() const {
    return nearest_clamp_;
  }
  // Shadow map lookups: compares against the stored depth (LessEqual), and
  // linear filtering makes the hardware average the 2x2 comparison results
  // (PCF). Clamped to an opaque white border, so positions outside the map
  // read as depth 1, i.e. lit. No mips.
  [[nodiscard]] id<MTLSamplerState> shadow() const { return shadow_; }

 private:
  // By TextureLodBiasStep().
  std::array<id<MTLSamplerState>, kTextureLodBiasStepCount> material_{};
  id<MTLSamplerState> linear_clamp_ = nil;
  id<MTLSamplerState> nearest_clamp_ = nil;
  id<MTLSamplerState> shadow_ = nil;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_SAMPLERS_H_
