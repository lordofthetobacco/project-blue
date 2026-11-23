#include "render/d3d12/staging_upload.h"

#include <d3d12.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "core/extent.h"
#include "core/log.h"
#include "render/d3d12/buffer.h"
#include "render/d3d12/d3d12_check.h"
#include "render/d3d12/d3d12_context.h"
#include "render/d3d12/format.h"

namespace blue::render::d3d12 {

namespace {

// Buffer pieces start at multiples of this (no copy needs more).
constexpr uint64_t kBufferAlignment = 16;
// Splitting a buffer copy into pieces smaller than this to fill the end of
// a chunk is not worth an extra copy command.
constexpr uint64_t kMinPieceBytes = uint64_t{64} << 10;

}  // namespace

StagingUploader::StagingUploader(const D3d12Context& context,
                                 uint64_t total_bytes)
    : context_(context),
      chunk_bytes_(std::clamp(
          AlignUp(total_bytes, kPlacementAlignment) + kPlacementAlignment,
          kPlacementAlignment, kChunkBytes)) {}

StagingUploader::~StagingUploader() { Finish(); }

ID3D12GraphicsCommandList* StagingUploader::cmd() {
  if (!recording_) {
    Begin(chunks_[current_]);
  }
  return chunks_[current_].list.Get();
}

void StagingUploader::CopyToBuffer(std::span<const std::byte> bytes,
                                   ID3D12Resource* buffer, uint64_t offset) {
  uint64_t done = 0;
  while (done < bytes.size_bytes()) {
    const uint64_t free_bytes =
        recording_ ? chunk_bytes_ - std::min(AlignUp(used_, kBufferAlignment),
                                             chunk_bytes_)
                   : chunk_bytes_;
    const uint64_t piece =
        std::min(bytes.size_bytes() - done,
                 free_bytes >= kMinPieceBytes ? free_bytes : chunk_bytes_);
    const uint64_t staged = Reserve(piece, kBufferAlignment);
    Chunk& chunk = chunks_[current_];
    std::memcpy(static_cast<std::byte*>(chunk.upload.mapped()) + staged,
                bytes.data() + done, piece);
    chunk.list->CopyBufferRegion(buffer, offset + done, chunk.upload.resource(),
                                 staged, piece);
    done += piece;
  }
}

void StagingUploader::CopyToTexture(std::span<const std::byte> packed,
                                    ID3D12Resource* texture,
                                    DXGI_FORMAT format, CopyBlock block,
                                    core::Extent2D extent, uint32_t level,
                                    uint32_t layer, uint32_t mip_levels) {
  const MipFootprint footprint = MipFootprintOf(block, extent, level);
  const size_t expected = size_t{footprint.row_bytes} * footprint.row_count;
  if (block.bytes == 0 || packed.size_bytes() != expected) {
    core::log::Fatal("Texture upload of {} bytes, expected {} (format {})",
                     packed.size_bytes(), expected, static_cast<int>(format));
  }
  if (footprint.row_pitch > chunk_bytes_) {
    core::log::Fatal("Texture row of {} bytes exceeds the staging chunk",
                     footprint.row_pitch);
  }
  const D3D12_TEXTURE_COPY_LOCATION destination{
      .pResource = texture,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
      .SubresourceIndex = level + layer * mip_levels,
  };
  // Footprints of block-compressed formats cover whole blocks.
  const uint32_t aligned_width = static_cast<uint32_t>(
      AlignUp(footprint.width, block.extent));

  uint32_t row = 0;
  while (row < footprint.row_count) {
    const uint64_t free_bytes =
        recording_
            ? chunk_bytes_ -
                  std::min(AlignUp(used_, kPlacementAlignment), chunk_bytes_)
            : chunk_bytes_;
    const uint64_t budget =
        free_bytes >= footprint.row_pitch ? free_bytes : chunk_bytes_;
    const auto count = static_cast<uint32_t>(std::min<uint64_t>(
        footprint.row_count - row, budget / footprint.row_pitch));
    const uint64_t staged =
        Reserve(uint64_t{footprint.row_pitch} * count, kPlacementAlignment);
    Chunk& chunk = chunks_[current_];
    auto* upload = static_cast<std::byte*>(chunk.upload.mapped()) + staged;
    for (uint32_t i = 0; i < count; ++i) {
      std::memcpy(upload + size_t{i} * footprint.row_pitch,
                  packed.data() + size_t{row + i} * footprint.row_bytes,
                  footprint.row_bytes);
    }
    const D3D12_TEXTURE_COPY_LOCATION source{
        .pResource = chunk.upload.resource(),
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = {.Offset = staged,
                            .Footprint = {.Format = format,
                                          .Width = aligned_width,
                                          .Height = count * block.extent,
                                          .Depth = 1,
                                          .RowPitch = footprint.row_pitch}},
    };
    chunk.list->CopyTextureRegion(&destination, 0, row * block.extent, 0,
                                  &source, nullptr);
    row += count;
  }
}

uint64_t StagingUploader::Reserve(uint64_t size, uint64_t alignment) {
  if (!recording_) {
    Begin(chunks_[current_]);
  }
  uint64_t offset = AlignUp(used_, alignment);
  if (offset + size > chunk_bytes_) {
    Submit(chunks_[current_]);
    current_ = (current_ + 1) % chunks_.size();
    Begin(chunks_[current_]);
    offset = 0;
  }
  used_ = offset + size;
  return offset;
}

void StagingUploader::Begin(Chunk& chunk) {
  ID3D12Device* device = context_.device();
  if (!chunk.list) {
    CheckHr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&chunk.allocator)));
    CheckHr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                      chunk.allocator.Get(), nullptr,
                                      IID_PPV_ARGS(&chunk.list)));
    chunk.list->SetName(L"blue staging upload");
    CheckHr(chunk.list->Close());
    chunk.upload = Buffer(context_, chunk_bytes_, Buffer::Access::kUpload,
                          L"blue staging chunk");
    staging_bytes_ += chunk_bytes_;
    context_.AddStagingBytes(chunk_bytes_);
  }
  if (chunk.fence_value != 0) {
    context_.WaitFor(chunk.fence_value);
    chunk.fence_value = 0;
  }
  CheckHr(chunk.allocator->Reset());
  CheckHr(chunk.list->Reset(chunk.allocator.Get(), nullptr));
  context_.descriptors().Bind(chunk.list.Get());
  recording_ = true;
  used_ = 0;
}

void StagingUploader::Submit(Chunk& chunk) {
  CheckHr(chunk.list->Close());
  chunk.fence_value = context_.Execute(chunk.list.Get());
  recording_ = false;
}

void StagingUploader::Finish() {
  if (finished_) {
    return;
  }
  finished_ = true;
  if (recording_) {
    Submit(chunks_[current_]);
  }
  for (Chunk& chunk : chunks_) {
    if (chunk.fence_value != 0) {
      context_.WaitFor(chunk.fence_value);
    }
    chunk = Chunk();  // Frees the upload buffer and the command list.
  }
  context_.RemoveStagingBytes(staging_bytes_);
  staging_bytes_ = 0;
  context_.PollValidationMessages();
}

}  // namespace blue::render::d3d12
