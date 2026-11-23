#ifndef BLUE_RENDER_D3D12_SAMPLERS_H_
#define BLUE_RENDER_D3D12_SAMPLERS_H_

#include <d3d12.h>

#include "render/d3d12/descriptor_heaps.h"

namespace blue::render::d3d12 {

class D3d12Context;

// The samplers shared by the whole backend, created once in the
// shader-visible sampler heap (design §4.1), with the same settings as the
// Vulkan backend's. A program's sampler table is one of these descriptors
// (each binding_layout.h set has one sampler). Neither copyable nor
// movable.
class Samplers {
 public:
  // `max_anisotropy` caps the material sampler's anisotropy (at most 16, the
  // Direct3D limit); 1 or less disables anisotropic filtering.
  Samplers(const D3d12Context& context, float max_anisotropy);

  Samplers(const Samplers&) = delete;
  Samplers& operator=(const Samplers&) = delete;

  // Material textures: trilinear, repeating, anisotropic when allowed.
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE material() const {
    return material_.gpu;
  }
  // Full-screen inputs and render targets: bilinear (and linear between
  // mips), clamped to the edge.
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE linear_clamp() const {
    return linear_clamp_.gpu;
  }
  // Exact texel fetches by UV, clamped to the edge.
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE nearest_clamp() const {
    return nearest_clamp_.gpu;
  }
  // The shadow map: bilinear depth comparison LESS_EQUAL, outside the map
  // white (lit).
  [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE shadow_compare() const {
    return shadow_compare_.gpu;
  }

 private:
  Descriptor material_;
  Descriptor linear_clamp_;
  Descriptor nearest_clamp_;
  Descriptor shadow_compare_;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_SAMPLERS_H_
