#include "render/encoders.h"

#include <string>

#include "render/metal_check.h"

namespace blue::render {

namespace {

// The stages of earlier encoders that write what a later encoder may read:
// render targets (fragment), copies and mip generation (blit) and the output
// of compute or machine-learning effects (dispatch, machine learning).
constexpr MTLStages kWritingStages = MTLStageFragment | MTLStageBlit |
                                     MTLStageDispatch | MTLStageMachineLearning;
// Every stage an encoder recorded by someone else may read with.
constexpr MTLStages kReadingStages = MTLStageVertex | MTLStageFragment |
                                     MTLStageBlit | MTLStageDispatch |
                                     MTLStageMachineLearning;

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

void SyncForExternalEncoding(const Recording& rec, id<MTLFence> fence,
                             const char* label) {
  // Its (empty) blit stage waits for everything before it...
  const id<MTL4ComputeCommandEncoder> encoder = BeginCopy(rec, label);
  // ...and every later encoder on the queue waits for it.
  [encoder barrierAfterStages:kWritingStages
            beforeQueueStages:kReadingStages
            visibilityOptions:MTL4VisibilityOptionDevice];
  if (fence != nil) {
    [encoder updateFence:fence afterEncoderStages:MTLStageBlit];
  }
  [encoder endEncoding];
}

}  // namespace blue::render
