#ifndef BLUE_RENDER_STAGING_UPLOAD_H_
#define BLUE_RENDER_STAGING_UPLOAD_H_

#include "render/metal_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/extent.h"
#include "render/buffer.h"
#include "render/metal_context.h"

namespace blue::render {

// The copy layout of a format: rows of blocks, each block `extent` x
// `extent` texels of `bytes` bytes (extent 1 for uncompressed formats).
struct CopyBlock {
  uint32_t extent = 1;
  uint32_t bytes = 0;
};

// Returns the copy layout of `format`; bytes is 0 for formats uploads do
// not handle (depth, packed, ...).
[[nodiscard]] CopyBlock CopyBlockOf(MTLPixelFormat format);

// Returns the bytes of mip `level` of an image of `format` whose mip 0 is
// `extent`, in its copy layout (whole blocks; 0 for unhandled formats).
[[nodiscard]] size_t CopyByteSize(MTLPixelFormat format, core::Extent2D extent,
                                  uint32_t level);

// Streams CPU data into private buffers and textures through a bounded
// amount of staging memory: two shared staging chunks of at most kChunkBytes
// each, used in turn, so the CPU fills one while the GPU copies from the
// other. Data larger than a chunk is split (buffers by range, image levels
// by rows of blocks). Copies are recorded into the current chunk's blit
// encoder, which blit() exposes for other blit commands that must run after
// them (mip generation); each chunk is its own command buffer, and the
// chunks are committed in order on the context's queue, so what one chunk
// records runs before the copies of the next. Finish() (or the destructor)
// commits the rest, blocks until the GPU is done and frees the staging
// memory. A GPU error is fatal. Neither copyable nor movable. Not
// thread-safe.
class StagingUploader {
 public:
  static constexpr size_t kChunkBytes = size_t{16} << 20;

  // `total_bytes` (the sum of what will be staged, may be approximate)
  // keeps the staging buffers small for small uploads.
  StagingUploader(const MetalContext& context, size_t total_bytes);
  ~StagingUploader();

  StagingUploader(const StagingUploader&) = delete;
  StagingUploader& operator=(const StagingUploader&) = delete;

  // Returns the blit encoder the next copy will be recorded into. Do not
  // keep it across a Copy* call, which may move on to the next chunk.
  [[nodiscard]] id<MTLBlitCommandEncoder> blit();

  // Records a copy of `bytes` to `buffer` at `offset`.
  void CopyToBuffer(std::span<const std::byte> bytes, id<MTLBuffer> buffer,
                    size_t offset);
  // Records a copy of `bytes`, mip `level` of slice `layer` (array layer or
  // cube face) of `texture`, in the copy layout of its format: tightly
  // packed rows of blocks, top to bottom. A size that does not match is
  // fatal.
  void CopyToImage(std::span<const std::byte> bytes, id<MTLTexture> texture,
                   uint32_t level, uint32_t layer);

  // Commits what is recorded, waits for the GPU and frees the staging
  // memory. Nothing may be recorded afterwards.
  void Finish();

 private:
  struct Chunk {
    Buffer staging;
    // The command buffer being recorded (with `blit` open on it) or, once
    // committed, the one whose completion frees the chunk for reuse.
    id<MTLCommandBuffer> cmd = nil;
    id<MTLBlitCommandEncoder> blit = nil;
    bool submitted = false;  // `cmd` is committed and not waited on yet.
  };

  // Returns the bytes still free in the current chunk after aligning; the
  // whole chunk when nothing is being recorded.
  [[nodiscard]] size_t FreeBytes() const;
  // Returns where `size` bytes (at most the chunk size) can be written in
  // the current chunk, aligned to `alignment`, moving on to the next chunk
  // when the current one is full.
  [[nodiscard]] size_t Reserve(size_t size, size_t alignment);
  // Waits until `chunk` is free and starts recording into it.
  void Begin(Chunk& chunk);
  void Submit(Chunk& chunk);
  // Blocks until the committed command buffer of `chunk` has completed.
  static void Wait(Chunk& chunk);

  const MetalContext& context_;
  size_t chunk_bytes_ = 0;
  std::array<Chunk, 2> chunks_;
  size_t current_ = 0;
  size_t used_ = 0;           // Bytes used in the current chunk.
  size_t staging_bytes_ = 0;  // Accounted in the context.
  bool recording_ = false;
  bool finished_ = false;
};

}  // namespace blue::render

#endif  // BLUE_RENDER_STAGING_UPLOAD_H_
