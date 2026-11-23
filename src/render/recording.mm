#include "render/recording.h"

#include <cstring>

#include "core/log.h"

namespace blue::render {

namespace {

// The strictest alignment of an MSL constant block (float4, float4x4).
constexpr size_t kAlignment = 16;

}  // namespace

TransientArena::TransientArena(const MetalContext& context, const char* label)
    : context_(context), label_(label) {}

void TransientArena::Reset() {
  chunk_ = 0;
  used_ = 0;
}

MTLGPUAddress TransientArena::Push(std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > kChunkBytes) {
    core::log::Fatal("Transient constants of {} bytes (limit {})",
                     bytes.size(), kChunkBytes);
  }
  size_t offset = (used_ + kAlignment - 1) / kAlignment * kAlignment;
  if (!chunks_.empty() && offset + bytes.size() > kChunkBytes) {
    ++chunk_;
    offset = 0;
  }
  if (chunk_ == chunks_.size()) {
    chunks_.emplace_back(context_, kChunkBytes,
                         Buffer::Access::kCpuSequentialWrite, label_);
  }
  const Buffer& chunk = chunks_[chunk_];
  std::memcpy(static_cast<std::byte*>(chunk.mapped()) + offset, bytes.data(),
              bytes.size());
  used_ = offset + bytes.size();
  return chunk.handle().gpuAddress + offset;
}

}  // namespace blue::render
