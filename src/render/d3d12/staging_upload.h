#ifndef BLUE_RENDER_D3D12_STAGING_UPLOAD_H_
#define BLUE_RENDER_D3D12_STAGING_UPLOAD_H_

#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "core/extent.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/format.h"

namespace blue::render::d3d12 {

class D3d12Context;

// Streams CPU data into default-heap buffers and textures through a bounded
// amount of upload memory: two upload chunks of at most kChunkBytes each,
// used in turn, so the CPU fills one while the GPU copies from the other
// (the same budget and contract as the Vulkan StagingUploader). Data larger
// than a chunk is split (buffers by range, texture levels by rows of
// blocks); texture rows are repacked to kRowPitchAlignment (MipFootprint).
// Commands are recorded into the current chunk's command list, which cmd()
// exposes for barriers around the copies; chunks execute in order on the
// context's queue, so barriers recorded in one chunk order the copies of the
// next. Finish() (or the destructor) executes the rest, blocks until the GPU
// is done and frees the upload memory. Neither copyable nor movable. Not
// thread-safe.
class StagingUploader {
 public:
  static constexpr uint64_t kChunkBytes = uint64_t{16} << 20;

  // `total_bytes` (the sum of what will be staged, may be approximate)
  // keeps the upload buffers small for small uploads.
  StagingUploader(const D3d12Context& context, uint64_t total_bytes);
  ~StagingUploader();

  StagingUploader(const StagingUploader&) = delete;
  StagingUploader& operator=(const StagingUploader&) = delete;

  // Returns the command list the next copy will be recorded into (the
  // descriptor heaps are bound).
  [[nodiscard]] ID3D12GraphicsCommandList* cmd();

  // Records a copy of `bytes` to `buffer` (a default-heap buffer in COMMON,
  // which the copy promotes to COPY_DEST) at `offset`.
  void CopyToBuffer(std::span<const std::byte> bytes, ID3D12Resource* buffer,
                    uint64_t offset);
  // Records a copy of `packed` (tightly packed rows of blocks of `block`,
  // top to bottom) to mip `level` of array layer `layer` of `texture`
  // (whose mip 0 is `extent`, with `mip_levels` mips, in COPY_DEST when the
  // copy executes; `format` is its resource format). A size that does not
  // match is fatal.
  void CopyToTexture(std::span<const std::byte> packed, ID3D12Resource* texture,
                     DXGI_FORMAT format, CopyBlock block, core::Extent2D extent,
                     uint32_t level, uint32_t layer, uint32_t mip_levels);

  // Executes what is recorded, waits for the GPU and frees the upload
  // memory. Nothing may be recorded afterwards.
  void Finish();

 private:
  struct Chunk {
    Buffer upload;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    uint64_t fence_value = 0;  // Pending while not reached; 0: never run.
  };

  // Returns where `size` bytes (at most the chunk size) can be written in
  // the current chunk, aligned to `alignment`, moving on to the next chunk
  // when the current one is full.
  [[nodiscard]] uint64_t Reserve(uint64_t size, uint64_t alignment);
  // Waits until `chunk` is free and starts recording into it.
  void Begin(Chunk& chunk);
  void Submit(Chunk& chunk);

  const D3d12Context& context_;
  uint64_t chunk_bytes_ = 0;
  std::array<Chunk, 2> chunks_;
  size_t current_ = 0;
  uint64_t used_ = 0;           // Bytes used in the current chunk.
  uint64_t staging_bytes_ = 0;  // Accounted in the context.
  bool recording_ = false;
  bool finished_ = false;
};

}  // namespace blue::render::d3d12

#endif  // BLUE_RENDER_D3D12_STAGING_UPLOAD_H_
