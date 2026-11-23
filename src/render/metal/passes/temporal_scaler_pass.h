#ifndef BLUE_RENDER_METAL_PASSES_TEMPORAL_SCALER_PASS_H_
#define BLUE_RENDER_METAL_PASSES_TEMPORAL_SCALER_PASS_H_

#include "render/metal/metal_types.h"

#import <MetalFX/MetalFX.h>

#include "render/metal/metal_context.h"
#include "render/metal/passes/frame_context.h"
#include "render/metal/render_targets.h"

namespace blue::render::metal {

// Temporal anti-aliasing and upscaling of the linear HDR scene with MetalFX
// (MTL4FXTemporalScaler): RenderTargets::hdr() at the render extent ->
// RenderTargets::upscaled() at the output extent, from the frame's jittered
// colour, its depth, its motion vectors and the reactive mask. With equal
// extents (Upscaling::kNative) it is plain temporal anti-aliasing.
//
// What the scaler is told (MTLFXTemporalScaler.h):
//   colour      linear HDR (RGBA16Float), not pre-exposed; the frame's
//               exposure (2^SceneView::post.exposure_ev, what the tonemapper
//               multiplies by) is passed as the exposure, so that the
//               scaler judges history in the range the image is shown in.
//   depth       reversed-Z (depthReversed).
//   motion      RenderTargets::motion_vectors(): previous minus current
//               position in units of the whole target, x right, y down,
//               without jitter; scaled to pixels by motionVectorScale =
//               render extent.
//   jitter      FrameContext::jitter as it is (render pixels, x right, y
//               down): the offset the frame was rasterized with.
//   reactive    RenderTargets::reactive_mask(): 1 = ignore history.
//   reset       on the first frame after Configure() and whenever the caller
//               says the previous frame is not this view's past.
//
// Borrows `context`, which must outlive it. Neither copyable nor movable.
// Not thread-safe.
class TemporalScalerPass {
 public:
  explicit TemporalScalerPass(const MetalContext& context);

  TemporalScalerPass(const TemporalScalerPass&) = delete;
  TemporalScalerPass& operator=(const TemporalScalerPass&) = delete;

  // Whether the device has a MetalFX temporal scaler for Metal 4. Without
  // one Configure() must only be given targets without temporal images.
  [[nodiscard]] bool supported() const { return supported_; }
  // The range of output / render size ratios (per axis) the device's scaler
  // supports; 1 and 1 when unsupported.
  [[nodiscard]] float min_scale() const { return min_scale_; }
  [[nodiscard]] float max_scale() const { return max_scale_; }

  // Creates the scaler for `targets` (render and output extents, formats),
  // or drops it when they have no temporal images. Call after the targets
  // were created or resized; the GPU must be idle, and no command buffer
  // that ever recorded the previous scaler may be committed again (see
  // RendererImpl::ConfigureTargets()). Blocks while MetalFX
  // compiles its pipelines. Failure to create the scaler is fatal.
  void Configure(const RenderTargets& targets);

  // Records the scaler for the frame of `ctx` (ctx.temporal must be true).
  // `reset` discards the history: pass true when the previous frame's image
  // and vectors do not belong to this frame's past (camera cut, skipped
  // frames). Every encoder recorded before is finished and visible to it.
  void Record(const FrameContext& ctx, bool reset);

 private:
  const MetalContext& context_;
  bool supported_ = false;
  float min_scale_ = 1.0f;
  float max_scale_ = 1.0f;
  // nil while off.
  id<MTL4FXTemporalScaler> scaler_ = nil;
  // Updated once the frame's inputs are complete; the scaler waits on it.
  id<MTLFence> inputs_ready_ = nil;
  // 1 x 1 R16Float holding the exposure, rewritten each frame.
  Image exposure_;
  // The next Record() starts a new history.
  bool first_frame_ = true;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_PASSES_TEMPORAL_SCALER_PASS_H_
