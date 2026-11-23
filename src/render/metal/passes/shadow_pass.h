#ifndef BLUE_RENDER_METAL_PASSES_SHADOW_PASS_H_
#define BLUE_RENDER_METAL_PASSES_SHADOW_PASS_H_

#include "render/metal/metal_types.h"

#include <memory>
#include <string>
#include <vector>

#include "render/metal/image.h"
#include "render/metal/mesh.h"
#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/pipeline.h"
#include "render/metal/shader_programs.h"

namespace blue::render::metal {

// Renders the sun's cascaded shadow map: one depth-only layer per cascade,
// every draw item whose world bounds reach the cascade. Alpha-masked
// materials cast solid shadows (the pass reads positions only).
//
// There is one map for all frames in flight: the barrier every render
// encoder starts with (render/encoders.h) orders this frame's depth writes
// after the previous frame's reads on the single queue. While shadows are off the map is bound but never sampled
// (FrameUniforms::shadow_params.x is 0).
//
// Borrows `programs` and `meshes`, which must outlive it. Neither copyable
// nor movable. Not thread-safe.
class ShadowPass {
 public:
  static constexpr MTLPixelFormat kFormat = MTLPixelFormatDepth16Unorm;

  ShadowPass(const MetalContext& context, ShaderPrograms& programs,
             const std::string& shader_dir, const std::vector<Mesh>& meshes);

  ShadowPass(const ShadowPass&) = delete;
  ShadowPass& operator=(const ShadowPass&) = delete;

  // Renders the cascades (one render encoder each) when ctx.shadows is
  // true; records nothing otherwise.
  void Record(const FrameContext& ctx) const;

  // The 2D-array texture of all cascades, sampled with
  // Samplers::shadow() (FrameGlobals::BindShadowMap()).
  [[nodiscard]] id<MTLTexture> map() const { return map_.texture(); }
  // GPU memory of the cascade map, in bytes.
  [[nodiscard]] uint64_t map_bytes() const { return map_.size_bytes(); }

 private:
  const MetalContext& context_;
  const std::vector<Mesh>& meshes_;
  Image map_;
  // Replaced when its shader is reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_SHADOW_PASS_H_
