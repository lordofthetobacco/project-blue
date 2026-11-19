#include "render/encoders.h"

#include <string>

#include "render/gpu_timer.h"
#include "render/metal_check.h"

namespace blue::render {

id<MTLRenderCommandEncoder> BeginRenderPass(const FrameContext& ctx,
                                            MTLRenderPassDescriptor* descriptor,
                                            const char* label) {
  ctx.timer.AttachSamples(descriptor);
  id<MTLRenderCommandEncoder> encoder =
      [ctx.cmd renderCommandEncoderWithDescriptor:descriptor];
  if (encoder == nil) {
    FatalIfNil(encoder, nil, std::string("render encoder ") + label);
  }
  encoder.label = @(label);
  return encoder;
}

id<MTLBlitCommandEncoder> BeginBlit(const FrameContext& ctx,
                                    const char* label) {
  // A blit pass descriptor (rather than plain blitCommandEncoder) is where
  // the timer's sample attachments go.
  MTLBlitPassDescriptor* descriptor =
      [MTLBlitPassDescriptor blitPassDescriptor];
  ctx.timer.AttachSamples(descriptor);
  id<MTLBlitCommandEncoder> encoder =
      [ctx.cmd blitCommandEncoderWithDescriptor:descriptor];
  if (encoder == nil) {
    FatalIfNil(encoder, nil, std::string("blit encoder ") + label);
  }
  encoder.label = @(label);
  return encoder;
}

}  // namespace blue::render
