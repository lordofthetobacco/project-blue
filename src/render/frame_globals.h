#ifndef BLUE_RENDER_FRAME_GLOBALS_H_
#define BLUE_RENDER_FRAME_GLOBALS_H_

#include "render/metal_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "render/buffer.h"
#include "render/frame_config.h"
#include "render/frame_uniforms.h"
#include "render/image.h"
#include "render/metal_context.h"
#include "render/samplers.h"

namespace blue::render {

// The views a frame renders with their own uniforms: the camera's and a
// rifle scope's (SceneView::scope).
enum class FrameView : uint32_t {
  kMain = 0,
  kScope = 1,
};
inline constexpr size_t kFrameViewCount = 2;

// The resources every scene shader sees (FrameResources in shaders/common.h),
// set on an encoder by Bind() (indices in shaders/bindings.h):
//   kBufferFrame (both stages):  FrameUniforms, one shared buffer per frame
//                                in flight and view
//   kTextureShadowMap:           cascaded shadow map (depth2d_array)
//   kTextureEnvironment:         prefiltered environment cube, specular IBL
//   kTextureBrdfLut:             BRDF lookup table (RG: scale, bias)
//   kTextureAmbientOcclusion:    ambient occlusion (R)
//   kSamplerShadow, kSamplerLinearClamp, kSamplerNearestClamp
// The environment and occlusion textures start as 1 x 1 defaults, so shaders
// can sample them before the passes that produce them exist: a black cube, a
// zero LUT and white occlusion. The shadow map has no default: the renderer
// binds ShadowPass's map before recording any frame. The scope view shares
// every texture but the ambient occlusion, which stays the white default
// (no SSAO in the scope picture).
// Borrows `samplers`, which must outlive it. Neither copyable nor movable.
// Not thread-safe.
class FrameGlobals {
 public:
  FrameGlobals(const MetalContext& context, const Samplers& samplers);

  FrameGlobals(const FrameGlobals&) = delete;
  FrameGlobals& operator=(const FrameGlobals&) = delete;

  // Copies `uniforms` into the buffer of `frame` and `view`, whose previous
  // use the GPU must have finished (its command buffer waited on).
  void Write(size_t frame, const FrameUniforms& uniforms,
             FrameView view = FrameView::kMain) const;

  // The Bind* calls choose what later Bind(encoder, ...) calls set; encoders
  // recorded before keep (and retain) what they were given, so these may be
  // called at any time between frames.
  // `shadow_map`: Depth*, 2D array with one layer per cascade.
  void BindShadowMap(id<MTLTexture> shadow_map);
  void BindEnvironment(id<MTLTexture> prefiltered_cube,
                       id<MTLTexture> brdf_lut);
  void BindAmbientOcclusion(id<MTLTexture> ambient_occlusion);

  // Sets the uniform buffer of `frame` and `view`, the textures and the
  // samplers on `encoder`. Call once per encoder: the bindings persist
  // across pipeline changes. A pass passes FrameContext::frame_index and
  // FrameContext::frame_view.
  void Bind(id<MTLRenderCommandEncoder> encoder, size_t frame,
            FrameView view = FrameView::kMain) const;

 private:
  static size_t Index(size_t frame, FrameView view) {
    return frame * kFrameViewCount + static_cast<size_t>(view);
  }

  const Samplers& samplers_;
  // Indexed by Index().
  std::array<Buffer, kFramesInFlight * kFrameViewCount> uniforms_;
  Image default_cube_;
  Image default_lut_;
  Image default_ambient_occlusion_;
  id<MTLTexture> shadow_map_ = nil;
  id<MTLTexture> environment_ = nil;
  id<MTLTexture> brdf_lut_ = nil;
  id<MTLTexture> ambient_occlusion_ = nil;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_FRAME_GLOBALS_H_
