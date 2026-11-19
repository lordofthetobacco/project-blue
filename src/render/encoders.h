#ifndef BLUE_RENDER_ENCODERS_H_
#define BLUE_RENDER_ENCODERS_H_

#include "render/metal_types.h"

#include "render/passes/frame_context.h"

// Every render and blit encoder of a frame is opened here, on the frame's
// command buffer (FrameContext::cmd), so that each gets its label (which names
// it in GPU captures) and the GPU timer's sample attachments for the current
// scope (GpuTimer::AttachSamples()). The caller ends the encoder
// (endEncoding) before opening the next one. A nil encoder is fatal: it means
// an invalid descriptor.

namespace blue::render {

// Opens a render pass described by `descriptor` (attachments, load and store
// actions); the GPU timer may add sample attachments to it. `label` must not
// be null.
[[nodiscard]] id<MTLRenderCommandEncoder> BeginRenderPass(
    const FrameContext& ctx, MTLRenderPassDescriptor* descriptor,
    const char* label);

// Opens a blit pass (copies, mip generation). `label` must not be null.
[[nodiscard]] id<MTLBlitCommandEncoder> BeginBlit(const FrameContext& ctx,
                                                  const char* label);

}  // namespace blue::render

#endif  // BLUE_RENDER_ENCODERS_H_
