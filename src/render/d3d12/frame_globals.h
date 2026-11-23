#ifndef BLUE_RENDER_D3D12_FRAME_GLOBALS_H_
#define BLUE_RENDER_D3D12_FRAME_GLOBALS_H_

#include <d3d12.h>

#include <array>
#include <cstdint>
#include <span>

#include "render/binding_layout.h"
#include "render/d3d12/image.h"
#include "render/d3d12/root_layout.h"
#include "render/frame_uniforms.h"

namespace blue::render::d3d12 {

class D3d12Context;
class Samplers;
class UploadRing;

// The root parameter indices of a scene root signature (SceneRootLayout()):
// the frame set and, when the layout has one, the pass set.
struct SceneRootParams {
  uint32_t frame_uniforms = 0;  // Root CBV.
  uint32_t frame_textures = 0;  // Table: shadow map, environment, LUT, AO.
  uint32_t shadow_sampler = 0;
  uint32_t linear_sampler = 0;
  // The pass set's table and its sampler table (UINT32_MAX: none).
  uint32_t pass_table = UINT32_MAX;
  uint32_t pass_sampler = UINT32_MAX;

  // Returns the indices of `layout`; `pass_set` is the pass set the layout
  // was made with (empty: none).
  [[nodiscard]] static SceneRootParams Of(
      const RootLayout& layout, std::span<const BindingDesc> pass_set);
};

// What one view of one frame binds as the frame set: its FrameUniforms in the
// upload ring and its texture table (transient, this frame only).
struct FrameBindings {
  D3D12_GPU_VIRTUAL_ADDRESS uniforms = 0;
  D3D12_GPU_DESCRIPTOR_HANDLE textures{};
};

// The frame set of the scene programs (binding_layout.h kFrameBindings): the
// counterpart of the Vulkan FrameGlobals.
//   b0  FrameUniforms (root CBV, per frame in the upload ring)
//   t1  cascaded shadow map (Texture2DArray<float>)
//   t2  prefiltered environment cube, specular IBL
//   t3  BRDF lookup table (RG: scale, bias)
//   t4  ambient occlusion (R)
//   s5  shadow comparison sampler, s6 linear clamp sampler
// t1-t4 start as 1 x 1 defaults (lit shadow map, black cube, zero LUT, white
// AO) so shaders can sample them before the passes that produce them exist.
// Borrows `context` and `samplers`. Neither copyable nor movable. Not
// thread-safe.
class FrameGlobals {
 public:
  FrameGlobals(const D3d12Context& context, const Samplers& samplers);

  FrameGlobals(const FrameGlobals&) = delete;
  FrameGlobals& operator=(const FrameGlobals&) = delete;

  // Points t2 and t3 at `prefiltered_cube` and `brdf_lut` (their staging
  // SRVs, read by every later Prepare()); they must be in a pixel shader
  // resource state whenever a frame samples them and outlive this object.
  void BindEnvironment(const Image& prefiltered_cube, const Image& brdf_lut);
  // Points t1 at the cascaded shadow map and t4 at the ambient occlusion
  // (same rules; call again when they are recreated).
  void BindShadowMap(const Image& shadow_map);
  void BindAmbientOcclusion(const Image& ambient_occlusion);

  // Copies `uniforms` into `ring` and writes the frame's texture table, for
  // the frame being recorded.
  [[nodiscard]] FrameBindings Prepare(const FrameUniforms& uniforms,
                                      UploadRing& ring) const;
  // Binds `frame` as the frame set of root signature params `params` (the
  // root signature must be set).
  void Bind(ID3D12GraphicsCommandList* cmd, const SceneRootParams& params,
            const FrameBindings& frame) const;

 private:
  const D3d12Context& context_;
  const Samplers& samplers_;
  Image default_shadow_map_;
  Image default_cube_;
  Image default_lut_;
  Image default_ambient_occlusion_;
  // The staging SRVs of t1-t4, in binding order.
  std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> textures_{};
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_FRAME_GLOBALS_H_
