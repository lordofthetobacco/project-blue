#ifndef BLUE_RENDER_METAL_RECORDING_H_
#define BLUE_RENDER_METAL_RECORDING_H_

#include "render/metal/metal_types.h"

#include <cstddef>
#include <span>
#include <vector>

#include "render/metal/buffer.h"
#include "render/metal/metal_context.h"

namespace blue::render::metal {

// Short-lived constant data for the commands of one command buffer: what
// the per-stage set...Bytes calls of older Metal copied into a command buffer.
// Metal 4 binds buffers by GPU address only, so small per-draw and per-pass
// blocks are pushed here and bound by the address Push() returns. A bump
// allocator over shared (CPU-written) chunks that it keeps between resets;
// it grows by a chunk when one is full. Neither copyable nor movable. Not
// thread-safe.
class TransientArena {
 public:
  // The size of a chunk, hence the largest block Push() takes.
  static constexpr size_t kChunkBytes = size_t{256} << 10;

  // Borrows `context`, which must outlive it. `label` names its buffers in
  // GPU captures.
  TransientArena(const MetalContext& context, const char* label);

  TransientArena(const TransientArena&) = delete;
  TransientArena& operator=(const TransientArena&) = delete;

  // Makes all the memory available again. The GPU must have finished every
  // command that used what was pushed before.
  void Reset();

  // Copies `bytes` (not empty, at most kChunkBytes) and returns the GPU
  // address of the copy, aligned to 16 bytes. The copy stays valid until
  // Reset().
  [[nodiscard]] MTLGPUAddress Push(std::span<const std::byte> bytes);

 private:
  const MetalContext& context_;
  const char* label_;
  std::vector<Buffer> chunks_;
  size_t chunk_ = 0;  // The chunk being filled.
  size_t used_ = 0;   // Bytes used in it.
};

// Where commands are being recorded: a frame's command buffer
// (FrameContext::rec) or the one of MetalContext::SubmitImmediate().
// Everything is borrowed and only valid during that recording.
struct Recording {
  // Open encoders on it through render/encoders.h.
  id<MTL4CommandBuffer> cmd = nil;
  // The renderer's one argument table (MetalContext::argument_table()),
  // already set on every render encoder that BeginRenderPass() opens. Bind
  // through the Bind*() helpers of render/encoders.h.
  id<MTL4ArgumentTable> args = nil;
  // Constants that live as long as the commands of this recording.
  TransientArena& constants;
};

}  // namespace blue::render::metal

#endif  // BLUE_RENDER_METAL_RECORDING_H_
