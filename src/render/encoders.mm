#include "render/encoders.h"

#include <string>

#include "render/metal_check.h"

namespace blue::render {

namespace {

// The stages of earlier encoders that write what a later encoder may read:
// render targets (fragment), copies and mip generation (blit) and compute
// output (dispatch).
constexpr MTLStages kWritingStages =
    MTLStageFragment | MTLStageBlit | MTLStageDispatch;

}  // namespace

id<MTL4RenderCommandEncoder> BeginRenderPass(
    const Recording& rec, MTL4RenderPassDescriptor* descriptor,
    const char* label) {
  id<MTL4RenderCommandEncoder> encoder =
      [rec.cmd renderCommandEncoderWithDescriptor:descriptor];
  FatalIfNil(encoder, nil, std::string("render encoder ") + label);
  encoder.label = @(label);
  [encoder barrierAfterQueueStages:kWritingStages
                      beforeStages:MTLStageFragment
                 visibilityOptions:MTL4VisibilityOptionDevice];
  [encoder setArgumentTable:rec.args
                   atStages:MTLRenderStageVertex | MTLRenderStageFragment];
  return encoder;
}

id<MTL4ComputeCommandEncoder> BeginCopy(const Recording& rec,
                                        const char* label) {
  id<MTL4ComputeCommandEncoder> encoder = [rec.cmd computeCommandEncoder];
  FatalIfNil(encoder, nil, std::string("copy encoder ") + label);
  encoder.label = @(label);
  [encoder barrierAfterQueueStages:kWritingStages
                      beforeStages:MTLStageBlit
                 visibilityOptions:MTL4VisibilityOptionDevice];
  return encoder;
}

}  // namespace blue::render
