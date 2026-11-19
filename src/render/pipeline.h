#ifndef BLUE_RENDER_PIPELINE_H_
#define BLUE_RENDER_PIPELINE_H_

#include "render/metal_types.h"

#include <memory>
#include <span>
#include <string>

#include "core/result.h"
#include "shader/shader_compiler.h"

namespace blue::render {

// How the fragment color is combined with the color attachment.
enum class BlendMode {
  kReplace,   // Overwrites the attachment.
  kAdditive,  // src + dst; alpha unchanged.
  kAlpha,     // src * src.a + dst * (1 - src.a); alpha unchanged.
};

// Describes a graphics pipeline. Everything is borrowed and only needs to
// stay valid for the duration of the Pipeline::Create() call.
struct GraphicsPipelineDesc {
  // Compiled on the device the pipeline is created for; the entry points are
  // "VertexMain" and "FragmentMain".
  const shader::ShaderLibrary* vertex = nullptr;
  // Null for a pipeline without a fragment stage (depth-only).
  const shader::ShaderLibrary* fragment = nullptr;
  // Values of the fragment function's function constants: element i is
  // [[function_constant(i)]] (indices in shaders/bindings.h).
  std::span<const bool> fragment_constants;

  // nil for a vertex stage without vertex buffers (e.g. a full-screen
  // triangle generated from the vertex id).
  MTLVertexDescriptor* vertex_descriptor = nil;
  // MTLPixelFormatInvalid means no color attachment (depth-only pipeline).
  MTLPixelFormat color_format = MTLPixelFormatInvalid;
  // MTLPixelFormatInvalid means no depth attachment and no depth test (e.g.
  // a full-screen pass).
  MTLPixelFormat depth_format = MTLPixelFormatInvalid;
  // The front face is always counter-clockwise.
  MTLCullMode cull_mode = MTLCullModeBack;
  // Equal lets a pass shade only the surfaces a depth pre-pass kept.
  MTLCompareFunction depth_compare = MTLCompareFunctionGreater;
  // False keeps the depth test but leaves the depth buffer untouched, e.g.
  // for overlays drawn after the opaque scene.
  bool depth_write = true;
  BlendMode blend = BlendMode::kReplace;
  // Clamps instead of clips fragments outside the near/far planes.
  bool depth_clamp = false;
  // The pipeline uses depth bias: the pass sets the factors itself
  // (setDepthBias:slopeScale:clamp:) after Bind(). Without it Bind() resets
  // the bias to zero, so a biased pipeline's factors never leak into the
  // next draws of the encoder.
  bool depth_bias = false;
  MTLPrimitiveTopologyClass topology = MTLPrimitiveTopologyClassTriangle;
  // Names the pipeline in GPU captures; nullptr gives a generic name.
  const char* label = nullptr;
};

// A graphics pipeline: the render pipeline state together with the
// encoder state Metal keeps outside of it (depth-stencil state, cull mode,
// winding, depth clip mode, depth bias), all applied by Bind(). Neither
// copyable nor movable (held by unique_ptr so a hot reload can swap it).
// Not thread-safe.
class Pipeline {
 public:
  // Creates the pipeline on `device`, which must be the device the shader
  // libraries were compiled on (the system default device; a mismatch is
  // fatal). Returns the Metal error text when a function is missing or the
  // stages do not link (e.g. varyings that stopped matching after a shader
  // edit): during a hot reload that is an ordinary failure.
  [[nodiscard]] static core::Result<std::unique_ptr<Pipeline>, std::string>
  Create(id<MTLDevice> device, const GraphicsPipelineDesc& desc);

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // Sets the pipeline and its fixed-function state on `encoder`.
  void Bind(id<MTLRenderCommandEncoder> encoder) const;

  [[nodiscard]] id<MTLRenderPipelineState> state() const { return state_; }
  [[nodiscard]] bool depth_bias() const { return depth_bias_; }

 private:
  Pipeline() = default;

  id<MTLRenderPipelineState> state_ = nil;
  // nil without a depth attachment: the encoder's default (no test, no
  // write) is what such a pipeline needs.
  id<MTLDepthStencilState> depth_stencil_ = nil;
  MTLCullMode cull_mode_ = MTLCullModeBack;
  MTLDepthClipMode depth_clip_mode_ = MTLDepthClipModeClip;
  bool depth_bias_ = false;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_PIPELINE_H_
