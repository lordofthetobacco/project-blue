#include "render/passes/temporal_scaler_pass.h"

#include <cmath>

#include "core/log.h"
#include "render/encoders.h"
#include "render/metal_check.h"

namespace blue::render {

namespace {

constexpr MTLPixelFormat kExposureFormat = MTLPixelFormatR16Float;

// Fatal unless `image` has every usage in `required`.
void CheckUsage(const Image& image, MTLTextureUsage required,
                const char* name) {
  if ((image.texture().usage & required) != required) {
    core::log::Fatal("Temporal scaler: the {} texture lacks usage {:#x}", name,
                     static_cast<unsigned long>(required));
  }
}

}  // namespace

TemporalScalerPass::TemporalScalerPass(const MetalContext& context)
    : context_(context) {
  const id<MTLDevice> device = context.device();
  supported_ = [MTLFXTemporalScalerDescriptor supportsMetal4FX:device];
  if (!supported_) {
    core::log::Warn("MetalFX temporal scaling is not available on {}: "
                    "upscaling modes fall back to off",
                    context.device_name());
    return;
  }
  min_scale_ =
      [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:device];
  max_scale_ =
      [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:device];
  inputs_ready_ = [device newFence];
  FatalIfNil(inputs_ready_, nil, "newFence");
  inputs_ready_.label = @"Temporal scaler inputs";
  exposure_ = Image(context, {.extent = {1, 1},
                              .format = kExposureFormat,
                              .usage = MTLTextureUsageRenderTarget |
                                       MTLTextureUsageShaderRead,
                              .label = "Scaler exposure"});
}

void TemporalScalerPass::Configure(const RenderTargets& targets) {
  scaler_ = nil;
  first_frame_ = true;
  if (!targets.temporal()) {
    return;
  }
  if (!supported_) {
    core::log::Fatal("Temporal render targets without a temporal scaler");
  }
  const core::Extent2D render = targets.render_extent();
  const core::Extent2D output = targets.output_extent();
  MTLFXTemporalScalerDescriptor* descriptor =
      [[MTLFXTemporalScalerDescriptor alloc] init];
  descriptor.colorTextureFormat = RenderTargets::kHdrFormat;
  descriptor.depthTextureFormat = RenderTargets::kDepthFormat;
  descriptor.motionTextureFormat = RenderTargets::kMotionVectorFormat;
  descriptor.outputTextureFormat = RenderTargets::kHdrFormat;
  descriptor.inputWidth = render.width;
  descriptor.inputHeight = render.height;
  descriptor.outputWidth = output.width;
  descriptor.outputHeight = output.height;
  descriptor.autoExposureEnabled = NO;
  descriptor.reactiveMaskTextureEnabled = YES;
  descriptor.reactiveMaskTextureFormat = RenderTargets::kReactiveMaskFormat;
  // Compiled now (the GPU is idle and the caller already stalls for the new
  // targets) rather than running a slower interim scaler for some frames.
  descriptor.requiresSynchronousInitialization = YES;
  scaler_ = [descriptor newTemporalScalerWithDevice:context_.device()
                                           compiler:context_.compiler()];
  FatalIfNil(scaler_, nil, "newTemporalScalerWithDevice:compiler:");

  CheckUsage(targets.hdr(), scaler_.colorTextureUsage, "colour");
  CheckUsage(targets.depth(), scaler_.depthTextureUsage, "depth");
  CheckUsage(targets.motion_vectors(), scaler_.motionTextureUsage, "motion");
  // (reactiveMaskTextureUsage, its newer name, needs macOS 27.)
  CheckUsage(targets.reactive_mask(), scaler_.reactiveTextureUsage,
             "reactive mask");
  CheckUsage(targets.upscaled(), scaler_.outputTextureUsage, "output");

  scaler_.depthReversed = YES;
  scaler_.motionVectorScaleX = static_cast<float>(render.width);
  scaler_.motionVectorScaleY = static_cast<float>(render.height);
  scaler_.inputContentWidth = render.width;
  scaler_.inputContentHeight = render.height;
  scaler_.fence = inputs_ready_;
  core::log::Info("Temporal scaler {} x {} -> {} x {}", render.width,
                  render.height, output.width, output.height);
}

void TemporalScalerPass::Record(const FrameContext& ctx, bool reset) {
  if (scaler_ == nil || !ctx.temporal) {
    core::log::Fatal("Temporal scaler recorded while off");
  }
  // The exposure: one texel, written on the GPU as MetalFX recommends, by
  // clearing it.
  MTL4RenderPassDescriptor* exposure =
      [[MTL4RenderPassDescriptor alloc] init];
  exposure.colorAttachments[0].texture = exposure_.texture();
  exposure.colorAttachments[0].loadAction = MTLLoadActionClear;
  exposure.colorAttachments[0].storeAction = MTLStoreActionStore;
  exposure.colorAttachments[0].clearColor =
      MTLClearColorMake(std::exp2(ctx.view.post.exposure_ev), 0.0, 0.0, 1.0);
  [BeginRenderPass(ctx.rec, exposure, "scaler exposure") endEncoding];

  const RenderTargets& targets = ctx.targets;
  scaler_.colorTexture = targets.hdr().texture();
  scaler_.depthTexture = targets.depth().texture();
  scaler_.motionTexture = targets.motion_vectors().texture();
  scaler_.reactiveMaskTexture = targets.reactive_mask().texture();
  scaler_.exposureTexture = exposure_.texture();
  scaler_.outputTexture = targets.upscaled().texture();
  // The header does not spell the sign out. Measured (static camera, image
  // difference between consecutive frames): the offset the scene was
  // rasterized with, in render pixels, x right and y down, is the stable
  // one; the opposite sign shimmers, worst when upscaling.
  scaler_.jitterOffsetX = ctx.jitter.x;
  scaler_.jitterOffsetY = ctx.jitter.y;
  scaler_.reset = reset || first_frame_;
  first_frame_ = false;

  // The scaler records its own encoders, without the barrier of
  // render/encoders.h.
  SyncForExternalEncoding(ctx.rec, inputs_ready_, "scaler inputs");
  [scaler_ encodeToCommandBuffer:ctx.rec.cmd];
}

}  // namespace blue::render
