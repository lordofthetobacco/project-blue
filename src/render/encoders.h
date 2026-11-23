#ifndef BLUE_RENDER_ENCODERS_H_
#define BLUE_RENDER_ENCODERS_H_

#include "render/metal_types.h"

#include <cstddef>
#include <span>

#include "render/recording.h"

// How passes record with the Metal 4 API. Everything a pass needs goes
// through this header (and FullscreenPass, FrameGlobals::Bind(),
// MaterialSystem::Bind(), Mesh, Pipeline::Bind(), which follow it), so that
// no pass handles argument tables, barriers or residency itself.
//
// Encoders
//   Open every encoder with BeginRenderPass() or BeginCopy() on a Recording:
//   `ctx.rec` in a frame, the argument of MetalContext::SubmitImmediate()
//   otherwise. End it with -endEncoding before opening the next one. There
//   is no blit encoder in Metal 4: copies and mip generation are commands of
//   an MTL4ComputeCommandEncoder (BeginCopy()).
//
// Barriers
//   Metal 4 tracks no resource: nothing orders an encoder that reads a
//   texture after the encoder that rendered or copied into it. Both Begin*()
//   functions therefore encode a consumer barrier first: the fragment stage
//   of a render encoder, and the copies of a copy encoder, wait until all
//   fragment, blit, dispatch and machine-learning work of the encoders
//   opened earlier on the queue has finished and is visible (barrierAfterQueueStages:
//   beforeStages:visibilityOptions:MTL4VisibilityOptionDevice). This covers
//   every case the renderer has: an attachment loaded after another pass
//   wrote it, a render target sampled by a later pass, a copy from a render
//   target, a texture sampled after a copy into it. Passes add no barriers.
//   What a pass may NOT do is read, in one encoder, a texture that the same
//   encoder renders to or copies into: use a second encoder. Vertex stages
//   are not held back: vertex shaders read only buffers the CPU wrote
//   (constants) or that an upload finished long before (meshes).
//
// Bindings
//   There are no per-stage setVertexBuffer / setFragmentTexture calls. One
//   argument table (Recording::args) serves the vertex and the fragment
//   stage of every encoder; BeginRenderPass() has set it. Bind with
//   BindBuffer(), BindConstants(), BindTexture() and BindSampler() at the
//   indices of shaders/bindings.h. Metal snapshots the table at each draw,
//   so: bind, then draw; what is bound stays bound for later draws and later
//   encoders until it is bound again (never rely on that across passes).
//   Constants that used to be set as bytes are pushed into the recording's
//   TransientArena by BindConstants(). Vertex buffers are bound like any
//   buffer, at kBufferVertices (Mesh::Bind()); the index buffer is an
//   argument of the draw (Mesh::DrawRange()).
//
// Residency and lifetime
//   Buffer and Image put themselves into the queue's residency set
//   (MetalContext). A command buffer keeps nothing alive: an object may only
//   be destroyed once the GPU has finished the last frame that used it (the
//   renderer's retire lists; MetalContext::WaitIdle() before replacing
//   render targets). Samplers and pipeline states need no residency.
//
// GPU timing
//   The renderer brackets each pass with GpuTimer scopes on the command
//   buffer; passes do nothing for it.

namespace blue::render {

// Opens a render pass described by `descriptor` (attachments, load and store
// actions) with the consumer barrier and the argument table described
// above. `label` must not be null. A nil encoder (an invalid descriptor) is
// fatal.
[[nodiscard]] id<MTL4RenderCommandEncoder> BeginRenderPass(
    const Recording& rec, MTL4RenderPassDescriptor* descriptor,
    const char* label);

// Opens an encoder for copies between buffers and textures and for mip
// generation, with the consumer barrier described above. `label` must not be
// null.
[[nodiscard]] id<MTL4ComputeCommandEncoder> BeginCopy(const Recording& rec,
                                                      const char* label);

// Makes everything recorded so far on the queue visible to work that is NOT
// encoded through this header and so has no consumer barrier of its own: an
// effect that records its own encoders into the command buffer (the MetalFX
// scaler). Records a small encoder that waits for all earlier work, holds
// back every stage of the encoders after it until then, and updates `fence`
// (handed to the effect, which waits on it) when non-nil. `label` must not
// be null.
void SyncForExternalEncoding(const Recording& rec, id<MTLFence> fence,
                             const char* label);

// Binds `texture` at texture index `index` (shaders/bindings.h).
inline void BindTexture(const Recording& rec, NSUInteger index,
                        id<MTLTexture> texture) {
  [rec.args setTexture:texture.gpuResourceID atIndex:index];
}

// Binds `sampler`, which must have been created with supportArgumentBuffers
// (render::Samplers are), at sampler index `index`.
inline void BindSampler(const Recording& rec, NSUInteger index,
                        id<MTLSamplerState> sampler) {
  [rec.args setSamplerState:sampler.gpuResourceID atIndex:index];
}

// Binds `buffer` from byte `offset` at buffer index `index`.
inline void BindBuffer(const Recording& rec, NSUInteger index,
                       id<MTLBuffer> buffer, size_t offset = 0) {
  [rec.args setAddress:buffer.gpuAddress + offset atIndex:index];
}

// Copies `bytes` into the recording's transient constants and binds the copy
// at buffer index `index`.
inline void BindBytes(const Recording& rec, NSUInteger index,
                      std::span<const std::byte> bytes) {
  [rec.args setAddress:rec.constants.Push(bytes) atIndex:index];
}

// BindBytes() of the object `block`, a constant struct mirrored in MSL.
template <typename T>
void BindConstants(const Recording& rec, NSUInteger index, const T& block) {
  BindBytes(rec, index, std::as_bytes(std::span(&block, 1)));
}

}  // namespace blue::render

#endif  // BLUE_RENDER_ENCODERS_H_
