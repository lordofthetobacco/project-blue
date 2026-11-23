#ifndef BLUE_RENDER_FULLSCREEN_PASS_H_
#define BLUE_RENDER_FULLSCREEN_PASS_H_

#include "render/metal_types.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/extent.h"
#include "render/metal_context.h"
#include "render/recording.h"
#include "render/pipeline.h"
#include "render/shader_programs.h"

namespace blue::render {

struct FullscreenPassDesc {
  // Fragment shader file; the vertex shader is shaders/fullscreen.vert.metal.
  std::string fragment_path;
  // Textures at kTextureInput0 .. kTextureInput0 + input_count - 1 (at most
  // kMaxInputTextures; 0 for a shader without inputs), all sampled with one
  // sampler at kSamplerInput.
  uint32_t input_count = 1;
  // Independent sets of inputs, chosen per Record() call (e.g. one per mip
  // of a bloom chain).
  uint32_t input_set_count = 1;
  // Constant block at kBufferPass of both stages, in bytes (at most
  // kMaxPassConstantBytes).
  uint32_t pass_constant_size = 0;
  MTLPixelFormat output_format = MTLPixelFormatInvalid;
  BlendMode blend = BlendMode::kReplace;
  // Names the pipeline and the render encoder in GPU captures.
  const char* label = "fullscreen";
};

// Where a FullscreenPass draws: one mip level and slice (array layer or
// cube face) of a color texture.
struct FullscreenTarget {
  id<MTLTexture> texture = nil;
  uint32_t level = 0;
  uint32_t slice = 0;
  // The size of `level`.
  core::Extent2D extent{};
  // Load keeps what is there (e.g. for additive blending); DontCare when
  // the triangle overwrites every pixel anyway.
  MTLLoadAction load_action = MTLLoadActionDontCare;
  // The pixels to draw (the scissor), inside `extent`; pixels outside keep
  // their contents (use MTLLoadActionLoad with it). An empty rectangle
  // draws the whole target. The viewport, hence varyings.uv, always spans
  // the whole target.
  MTLScissorRect area{};
};

// A pass that runs one fragment shader over a whole target: tonemapping,
// copies, bloom, SSAO, FXAA. Owns its pipeline (hot-reloaded through
// `programs`) and the input sets. The fragment shader sees varyings.uv in
// [0, 1] with (0, 0) at the top-left pixel corner.
//
// Each Record() opens and ends its own render encoder through
// render/encoders.h, so its inputs may be what the encoder before it
// rendered. Borrows `context` and `programs`, which must outlive it. Neither copyable nor movable. Not
// thread-safe.
class FullscreenPass {
 public:
  FullscreenPass(const MetalContext& context, ShaderPrograms& programs,
                 const std::string& shader_dir, const FullscreenPassDesc& desc);

  FullscreenPass(const FullscreenPass&) = delete;
  FullscreenPass& operator=(const FullscreenPass&) = delete;

  // Points input set `set_index` at `textures` (input_count of them),
  // sampled with `sampler`. The pass keeps strong references until the set
  // is pointed elsewhere, but that does not keep the textures resident: the
  // owner of the textures keeps them alive while a frame in flight uses
  // them (render targets are replaced only when the GPU is idle).
  void SetInputs(uint32_t set_index,
                 std::span<const id<MTLTexture> __strong> textures,
                 id<MTLSamplerState> sampler);

  // Records rendering into `target` with input set `set_index` on `rec`: a
  // frame's (FrameContext::rec) or the one of
  // MetalContext::SubmitImmediate(). `constants` must be empty or exactly
  // pass_constant_size bytes.
  void Record(const Recording& rec, const FullscreenTarget& target,
              uint32_t set_index = 0,
              std::span<const std::byte> constants = {}) const;

 private:
  struct InputSet {
    std::vector<id<MTLTexture>> textures;
    id<MTLSamplerState> sampler = nil;
  };

  // Returns the render pass that renders into `target`.
  [[nodiscard]] static MTL4RenderPassDescriptor* DescribeTarget(
      const FullscreenTarget& target);

  FullscreenPassDesc desc_;
  std::vector<InputSet> sets_;
  // Replaced when its shaders are reloaded; never null after construction.
  std::unique_ptr<Pipeline> pipeline_;
};

// Returns the bytes of a pass constant block, for FullscreenPass::Record().
template <typename T>
[[nodiscard]] std::span<const std::byte> ConstantBytes(const T& block) {
  return std::as_bytes(std::span(&block, 1));
}

}  // namespace blue::render

#endif  // BLUE_RENDER_FULLSCREEN_PASS_H_
